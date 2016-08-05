//===-- ValueEnumerator.cpp - Number values and types for bitcode writer --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the ValueEnumerator class.
//
//===----------------------------------------------------------------------===//

#include "ValueEnumerator.h"
#include "LLVMBitCodes.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
using namespace llvm;
using namespace ver_3_1;

static bool isIntegerValue(const std::pair<const Value *, unsigned> &V) {
  return V.first->getType()->isIntegerTy();
}

/// ValueEnumerator - Enumerate module-level information.
ValueEnumerator::ValueEnumerator(const Module *M) : M(*M), C(M->getContext()) {
  // Enumerate the global variables.
  for (auto &G : M->globals())
    EnumerateValue(&G);

  // Enumerate the functions.
  for (auto &F : *M) {
    EnumerateValue(&F);
    EnumerateAttributes(cast<Function>(F).getAttributes());
    MapDebugInfo(&F);
  }

  // Enumerate the aliases.
  for (auto &A : M->aliases())
    EnumerateValue(&A);

  // Remember what is the cutoff between globalvalue's and other constants.
  unsigned FirstConstant = Values.size();

  // Enumerate the global variable initializers.
  for (Module::const_global_iterator I = M->global_begin(), E = M->global_end();
       I != E; ++I)
    if (I->hasInitializer())
      EnumerateValue(I->getInitializer());

  // Enumerate the aliasees.
  for (Module::const_alias_iterator I = M->alias_begin(), E = M->alias_end();
       I != E; ++I)
    EnumerateValue(I->getAliasee());

  // Insert constants and metadata that are named at module level into the slot
  // pool so that the module symbol table can refer to them...
  EnumerateValueSymbolTable(M->getValueSymbolTable());
  EnumerateNamedMetadata(M);

  SmallVector<std::pair<unsigned, MDNode *>, 8> MDs;

  // Enumerate types used by function bodies and argument lists.
  for (const auto &F : *M) {

    for (const auto &A : F.args())
      EnumerateType(A.getType());

    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB) {
        for (const Use &Op : I.operands()) {
          auto *MD = dyn_cast<MetadataAsValue>(&Op);
          if (!MD) {
            EnumerateOperandType(Op);
            continue;
          }

          // Local metadata is enumerated during function-incorporation.
          if (isa<LocalAsMetadata>(MD->getMetadata()))
            continue;

          EnumerateMetadata(MD->getMetadata());
        }

        EnumerateType(I.getType());
        if (const CallInst *CI = dyn_cast<CallInst>(&I))
          EnumerateAttributes(CI->getAttributes());
        else if (const InvokeInst *II = dyn_cast<InvokeInst>(&I))
          EnumerateAttributes(II->getAttributes());

        // Enumerate metadata attached with this instruction.
        MDs.clear();
        I.getAllMetadataOtherThanDebugLoc(MDs);
        for (unsigned i = 0, e = MDs.size(); i != e; ++i)
          EnumerateMetadata(MDs[i].second);

        // Don't enumerate the location directly -- it has a special record
        // type -- but enumerate its operands.
        if (DILocation *L = I.getDebugLoc())
          EnumerateMDNodeOperands(L);
      }
  }

  // Optimize constant ordering.
  OptimizeConstants(FirstConstant, Values.size());
}

unsigned ValueEnumerator::getInstructionID(const Instruction *Inst) const {
  InstructionMapType::const_iterator I = InstructionMap.find(Inst);
  assert(I != InstructionMap.end() && "Instruction is not mapped!");
  return I->second;
}

void ValueEnumerator::setInstructionID(const Instruction *I) {
  InstructionMap[I] = InstructionCount++;
}

unsigned ValueEnumerator::getValueID(const Value *V) const {
  if (auto *MD = dyn_cast<MetadataAsValue>(V))
    return getMetadataID(MD->getMetadata());

  ValueMapType::const_iterator I = ValueMap.find(V);
  assert(I != ValueMap.end() && "Value not in slotcalculator!");
  return I->second - 1;
}

void ValueEnumerator::dump() const {
  print(dbgs(), ValueMap, "Default");
  dbgs() << '\n';
  print(dbgs(), MDValueMap, "MetaData");
  dbgs() << '\n';
}

void ValueEnumerator::print(raw_ostream &OS, const ValueMapType &Map,
                            const char *Name) const {

  OS << "Map Name: " << Name << "\n";
  OS << "Size: " << Map.size() << "\n";
  for (ValueMapType::const_iterator I = Map.begin(), E = Map.end(); I != E;
       ++I) {

    const Value *V = I->first;
    if (V->hasName())
      OS << "Value: " << V->getName();
    else
      OS << "Value: [null]\n";
    V->dump();

    OS << " Uses(" << std::distance(V->use_begin(), V->use_end()) << "):";
    for (Value::const_use_iterator UI = V->use_begin(), UE = V->use_end();
         UI != UE; ++UI) {
      if (UI != V->use_begin())
        OS << ",";
      if ((*UI)->hasName())
        OS << " " << (*UI)->getName();
      else
        OS << " [null]";
    }
    OS << "\n\n";
  }
}

void ValueEnumerator::print(raw_ostream &OS, const MetadataMapType &Map,
                            const char *Name) const {
  OS << "Map Name: " << Name << "\n";
  OS << "Size: " << Map.size() << "\n";
  for (auto I = Map.begin(), E = Map.end(); I != E; ++I) {
    const llvm::Metadata *MD = I->first;
    OS << "Metadata: slot = " << I->second << "\n";
    MD->print(OS);
  }
}

// Optimize constant ordering.
namespace {
struct CstSortPredicate {
  ValueEnumerator &VE;
  explicit CstSortPredicate(ValueEnumerator &ve) : VE(ve) {}
  bool operator()(const std::pair<const Value *, unsigned> &LHS,
                  const std::pair<const Value *, unsigned> &RHS) {
    // Sort by plane.
    if (LHS.first->getType() != RHS.first->getType())
      return VE.getTypeID(LHS.first->getType()) <
             VE.getTypeID(RHS.first->getType());
    // Then by frequency.
    return LHS.second > RHS.second;
  }
};
}

/// OptimizeConstants - Reorder constant pool for denser encoding.
void ValueEnumerator::OptimizeConstants(unsigned CstStart, unsigned CstEnd) {
  if (CstStart == CstEnd || CstStart + 1 == CstEnd)
    return;

  CstSortPredicate P(*this);
  std::stable_sort(Values.begin() + CstStart, Values.begin() + CstEnd, P);

  // Ensure that integer constants are at the start of the constant pool.  This
  // is important so that GEP structure indices come before gep constant exprs.
  std::partition(Values.begin() + CstStart, Values.begin() + CstEnd,
                 isIntegerValue);

  // Rebuild the modified portion of ValueMap.
  for (; CstStart != CstEnd; ++CstStart)
    ValueMap[Values[CstStart].first] = CstStart + 1;
}

void ValueEnumerator::MapDebugInfo(const Function *F) {
  auto *DI = F->getSubprogram();

  if (DI == nullptr)
    return;

  bool Inserted = DIMap.insert(std::make_pair(DI, F)).second;
  assert(Inserted && "Subprogram exists?");
  (void)Inserted;
}

void ValueEnumerator::EnumerateInt(uint64_t Val, unsigned NumBits) {
  auto *T = IntegerType::get(C, NumBits);
  auto *V = ConstantInt::get(T, Val);
  EnumerateValue(V);
}

void ValueEnumerator::EnumerateInt32(uint32_t Val) { EnumerateInt(Val, 32); }

void ValueEnumerator::EnumerateInt64(uint64_t Val) { EnumerateInt(Val, 64); }

void ValueEnumerator::EnumerateBool(bool b) { EnumerateInt(b ? 1 : 0, 1); }

void ValueEnumerator::EnumerateDwarfTag(unsigned Tag) {
  EnumerateInt32(LLVMDebugVersion + Tag);
}

void ValueEnumerator::EnumerateMDString(StringRef S) {
  EnumerateMetadata(MDString::get(C, S));
}

void ValueEnumerator::EnumerateDICompositeTypeBase(const DIType *MD,
                                                   const Metadata *ParentType,
                                                   const Metadata *MemberTypes,
                                                   uint32_t RuntimeLanguage) {
  //! 6 = metadata !{
  //  i32,      ;; Tag (see below)
  EnumerateDwarfTag(MD->getTag());
  //  metadata, ;; Reference to context
  EnumerateMetadata(MD->getRawScope());
  //  metadata, ;; Name (may be "" for anonymous types)
  EnumerateMetadata(MD->getRawName());
  //  metadata, ;; Reference to file where defined (may be NULL)
  EnumerateMetadata(MD->getRawFile());
  //  i32,      ;; Line number where defined (may be 0)
  EnumerateInt32(MD->getLine());
  //  i64,      ;; Size in bits
  EnumerateInt64(MD->getSizeInBits());
  //  i64,      ;; Alignment in bits
  EnumerateInt64(MD->getAlignInBits());
  //  i64,      ;; Offset in bits
  EnumerateInt64(MD->getOffsetInBits());
  //  i32,      ;; Flags
  EnumerateInt32(MD->getFlags());
  //  metadata, ;; Reference to type derived from
  EnumerateMetadata(ParentType);
  //  metadata, ;; Reference to array of member descriptors
  EnumerateMetadata(MemberTypes);
  //  i32       ;; Runtime languages
  EnumerateInt32(RuntimeLanguage);
  //}
}

void ValueEnumerator::EnumerateMDTuple(const MDTuple *MD) {
  EnumerateMDNodeOperands(MD);
}

void ValueEnumerator::EnumerateDILocation(const DILocation *MD) {}

void ValueEnumerator::EnumerateDIExpression(const DIExpression *MD) {
  // Do not enumerate any operand.
}

void ValueEnumerator::EnumerateGenericDINode(const GenericDINode *MD) {}

void ValueEnumerator::EnumerateDISubrange(const DISubrange *MD) {}
void ValueEnumerator::EnumerateDIEnumerator(const DIEnumerator *MD) {}

void ValueEnumerator::EnumerateDIBasicType(const DIBasicType *MD) {
  //! 4 = metadata !{
  //  i32,      ;; Tag = 36 + LLVMDebugVersion
  EnumerateDwarfTag(MD->getTag());
  //            ;; (DW_TAG_base_type)
  //  metadata, ;; Reference to context
  EnumerateMetadata(MD->getRawScope());
  //  metadata, ;; Name (may be "" for anonymous types)
  EnumerateMetadata(MD->getRawName());
  //  metadata, ;; Reference to file where defined (may be NULL)
  EnumerateMetadata(MD->getRawFile());
  //  i32,      ;; Line number where defined (may be 0)
  EnumerateInt32(MD->getLine());
  //  i64,      ;; Size in bits
  EnumerateInt64(MD->getSizeInBits());
  //  i64,      ;; Alignment in bits
  EnumerateInt64(MD->getAlignInBits());
  //  i64,      ;; Offset in bits
  EnumerateInt64(MD->getOffsetInBits());
  //  i32,      ;; Flags
  EnumerateInt32(MD->getFlags());
  //  i32       ;; DWARF type encoding
  EnumerateInt32(MD->getEncoding());
  //}
}

void ValueEnumerator::EnumerateDIDerivedType(const DIDerivedType *MD) {}

void ValueEnumerator::EnumerateDICompositeType(const DICompositeType *MD) {}

void ValueEnumerator::EnumerateDISubroutineType(const DISubroutineType *MD) {
  EnumerateDICompositeTypeBase(MD, nullptr, MD->getRawTypeArray(), 0);
}

void ValueEnumerator::EnumerateDIFile(const DIFile *MD) { 
  //!0 = metadata !{
  //  i32,       ;; Tag = 41 + LLVMDebugVersion
  //             ;; (DW_TAG_file_type)
  EnumerateDwarfTag(MD->getTag());
  //  metadata,  ;; Source file name
  EnumerateMetadata(MD->getRawFilename());
  //  metadata,  ;; Source file directory (includes trailing slash)
  EnumerateMetadata(MD->getRawDirectory());
  //  metadata   ;; Unused
  EnumerateMetadata(nullptr);
  //}
}

void ValueEnumerator::EnumerateDICompileUnit(const DICompileUnit *MD) {
  //! 0 = metadata !{
  //  i32,       ;; Tag = 17 + LLVMDebugVersion
  //             ;; (DW_TAG_compile_unit)
  EnumerateDwarfTag(MD->getTag());
  //  i32,       ;; Unused field.
  EnumerateInt32(0);
  //  i32,       ;; DWARF language identifier (ex. DW_LANG_C89)
  EnumerateInt32(MD->getSourceLanguage());
  //  metadata,  ;; Source file name
  EnumerateMDString(MD->getFilename());
  //  metadata,  ;; Source file directory (includes trailing slash)
  EnumerateMDString(MD->getDirectory());
  //  metadata   ;; Producer (ex. "4.0.1 LLVM (LLVM research group)")
  EnumerateMDString(MD->getProducer());
  //  i1,        ;; True if this is a main compile unit.
  EnumerateBool(true);
  //  i1,        ;; True if this is optimized.
  EnumerateBool(MD->isOptimized());
  //  metadata,  ;; Flags
  EnumerateMetadata(MD->getRawFlags());
  //  i32        ;; Runtime version
  EnumerateInt32(MD->getRuntimeVersion());
  //  metadata   ;; List of enums types
  EnumerateMetadata(MD->getRawEnumTypes());
  //  metadata   ;; List of retained types
  EnumerateMetadata(MD->getRawRetainedTypes());
  //  metadata   ;; List of subprograms
  EnumerateMetadata(nullptr);
  //  metadata   ;; List of global variables
  EnumerateMetadata(MD->getRawGlobalVariables());
  //}
}

void ValueEnumerator::EnumerateDISubprogram(const DISubprogram *MD) {
  //! 2 = metadata !{
  //  i32,      ;; Tag = 46 + LLVMDebugVersion
  EnumerateDwarfTag(MD->getTag());
  //            ;; (DW_TAG_subprogram)
  //  i32,      ;; Unused field.
  EnumerateInt32(0);
  //  metadata, ;; Reference to context descriptor
  EnumerateMetadata(MD->getRawScope());
  //  metadata, ;; Name
  EnumerateMetadata(MD->getRawName());
  //  metadata, ;; Display name (fully qualified C++ name)
  EnumerateMDString(MD->getDisplayName());
  //  metadata, ;; MIPS linkage name (for C++)
  EnumerateMetadata(MD->getRawLinkageName());
  //  metadata, ;; Reference to file where defined
  EnumerateMetadata(MD->getRawFile());
  //  i32,      ;; Line number where defined
  EnumerateInt32(MD->getLine());
  //  metadata, ;; Reference to type descriptor
  EnumerateMetadata(MD->getRawType());
  //  i1,       ;; True if the global is local to compile unit (static)
  EnumerateBool(MD->isLocalToUnit());
  //  i1,       ;; True if the global is defined in the compile unit (not
  //  extern)
  EnumerateBool(MD->isDefinition());
  //  i32,      ;; Line number where the scope of the subprogram begins
  EnumerateInt32(MD->getScopeLine());
  //  i32,      ;; Virtuality, e.g. dwarf::DW_VIRTUALITY__virtual
  EnumerateInt32(MD->getVirtuality());
  //  i32,      ;; Index into a virtual function
  EnumerateInt32(MD->getVirtualIndex());
  //  metadata, ;; indicates which base type contains the vtable pointer for the
  //            ;; derived class
  EnumerateMetadata(MD->getRawContainingType());
  //  i32,      ;; Flags - Artifical, Private, Protected, Explicit, Prototyped.
  EnumerateInt32(MD->getFlags());
  //  i1,       ;; isOptimized
  EnumerateBool(MD->isOptimized());
  //  Function *,;; Pointer to LLVM function
  EnumerateValue(getFunction(MD));
  //  metadata, ;; Lists function template parameters
  EnumerateMetadata(MD->getRawTemplateParams());
  //  metadata  ;; Function declaration descriptor
  EnumerateMetadata(MD->getRawDeclaration());
  //  metadata  ;; List of function variables
  EnumerateMetadata(MD->getRawVariables());
  //}
}

void ValueEnumerator::EnumerateDILexicalBlock(const DILexicalBlock *MD) {}
void ValueEnumerator::EnumerateDILexicalBlockFile(
    const DILexicalBlockFile *MD) {}
void ValueEnumerator::EnumerateDINamespace(const DINamespace *MD) {}
void ValueEnumerator::EnumerateDIModule(const DIModule *MD) {}
void ValueEnumerator::EnumerateDITemplateTypeParameter(
    const DITemplateTypeParameter *MD) {}
void ValueEnumerator::EnumerateDITemplateValueParameter(
    const DITemplateValueParameter *MD) {}
void ValueEnumerator::EnumerateDIGlobalVariable(const DIGlobalVariable *MD) {}

void ValueEnumerator::EnumerateDILocalVariable(const DILocalVariable *MD) {
  //! 7 = metadata !{
  //  i32,      ;; Tag (see below)
  EnumerateDwarfTag(MD->getTag());
  //  metadata, ;; Context
  EnumerateMetadata(MD->getRawScope());
  //  metadata, ;; Name
  EnumerateMetadata(MD->getRawName());
  //  metadata, ;; Reference to file where defined
  EnumerateMetadata(MD->getRawFile());
  //  i32,      ;; 24 bit - Line number where defined
  //            ;; 8 bit - Argument number. 1 indicates 1st argument.
  //  metadata, ;; Type descriptor
  uint32_t LineNo = MD->getLine();
  uint32_t ArgNo = MD->getArg();
  EnumerateInt32((LineNo | (ArgNo << 24)));
  //  i32,      ;; flags
  EnumerateInt32(MD->getFlags());
  //  metadata  ;; (optional) Reference to inline location
  EnumerateMetadata(nullptr);
  //}
}

void ValueEnumerator::EnumerateDIObjCProperty(const DIObjCProperty *MD) {}
void ValueEnumerator::EnumerateDIImportedEntity(const DIImportedEntity *MD) {}
void ValueEnumerator::EnumerateDIMacro(const DIMacro *MD) {}
void ValueEnumerator::EnumerateDIMacroFile(const DIMacroFile *MD) {}

/// EnumerateValueSymbolTable - Insert all of the values in the specified symbol
/// table into the values table.
void ValueEnumerator::EnumerateValueSymbolTable(const ValueSymbolTable &VST) {
  for (ValueSymbolTable::const_iterator VI = VST.begin(), VE = VST.end();
       VI != VE; ++VI)
    EnumerateValue(VI->getValue());
}

/// EnumerateNamedMetadata - Insert all of the values referenced by
/// named metadata in the specified module.
void ValueEnumerator::EnumerateNamedMetadata(const Module *M) {
  for (auto &I : M->named_metadata())
    EnumerateNamedMDNode(&I);
}

void ValueEnumerator::EnumerateNamedMDNode(const NamedMDNode *MD) {
  for (unsigned i = 0, e = MD->getNumOperands(); i != e; ++i)
    EnumerateMetadata(MD->getOperand(i));
}

/// EnumerateMDNodeOperands - Enumerate all non-function-local values
/// and types referenced by the given MDNode.
void ValueEnumerator::EnumerateMDNodeOperands(const MDNode *N) {
  for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i) {
    Metadata *MD = N->getOperand(i);
    if (!MD)
      continue;
    assert(!isa<LocalAsMetadata>(MD) && "MDNodes cannot be function-local");
    EnumerateMetadata(MD);
  }
}

void ValueEnumerator::EnumerateMDNode(const MDNode *MD) {
  switch (MD->getMetadataID()) {
  default:
    llvm_unreachable("Invalid MDNode subclass");
#define HANDLE_SPECIALIZED_MDNODE_LEAF(CLASS)                                  \
  case Metadata::CLASS##Kind:                                                  \
    Enumerate##CLASS(cast<CLASS>(MD));                                         \
    break;
#define HANDLE_MDNODE_LEAF(CLASS)                                              \
  case Metadata::CLASS##Kind:                                                  \
    Enumerate##CLASS(cast<CLASS>(MD));                                         \
    break;
#include "llvm/IR/Metadata.def"
  }
}

void ValueEnumerator::EnumerateMetadata(const Metadata *MD) {
  if (MD == nullptr)
    return;

  assert(
      (isa<MDNode>(MD) || isa<MDString>(MD) || isa<ConstantAsMetadata>(MD)) &&
      "Invalid metadata kind");

  // Insert a dummy ID to block the co-recursive call to
  // EnumerateMDNodeOperands() from re-visiting MD in a cyclic graph.
  //
  // Return early if there's already an ID.
  if (!MDValueMap.insert(std::make_pair(MD, 0)).second)
    return;

  // Visit operands first to minimize RAUW.
  if (auto *N = dyn_cast<MDNode>(MD))
    EnumerateMDNode(N);
  else if (auto *C = dyn_cast<ConstantAsMetadata>(MD))
    EnumerateValue(C->getValue());

  HasMDString |= isa<MDString>(MD);
  HasDILocation |= isa<DILocation>(MD);

  // Replace the dummy ID inserted above with the correct one.  MDValueMap may
  // have changed by inserting operands, so we need a fresh lookup here.
  MDs.push_back(MD);
  MDValueMap[MD] = MDs.size();
}

/// EnumerateFunctionLocalMetadataa - Incorporate function-local metadata
/// information reachable from the given MDNode.
void ValueEnumerator::EnumerateFunctionLocalMetadata(
    const LocalAsMetadata *Local) {
  // Check to see if it's already in!
  unsigned &MDValueID = MDValueMap[Local];
  if (MDValueID)
    return;

  MDs.push_back(Local);
  MDValueID = MDs.size();

  EnumerateValue(Local->getValue());

  // Also, collect all function-local metadata for easy access.
  FunctionLocalMDs.push_back(Local);
}

void ValueEnumerator::EnumerateValue(const Value *V) {
  assert(!V->getType()->isVoidTy() && "Can't insert void values!");
  assert(!isa<MetadataAsValue>(V) && "EnumerateValue doesn't handle Metadata!");

  // Check to see if it's already in!
  unsigned &ValueID = ValueMap[V];
  if (ValueID) {
    // Increment use count.
    Values[ValueID - 1].second++;
    return;
  }

  // Enumerate the type of this value.
  EnumerateType(V->getType());

  if (const Constant *C = dyn_cast<Constant>(V)) {
    if (isa<GlobalValue>(C)) {
      // Initializers for globals are handled explicitly elsewhere.
    } else if (C->getNumOperands()) {
      // If a constant has operands, enumerate them.  This makes sure that if a
      // constant has uses (for example an array of const ints), that they are
      // inserted also.

      // We prefer to enumerate them with values before we enumerate the user
      // itself.  This makes it more likely that we can avoid forward references
      // in the reader.  We know that there can be no cycles in the constants
      // graph that don't go through a global variable.
      for (User::const_op_iterator I = C->op_begin(), E = C->op_end(); I != E;
           ++I)
        if (!isa<BasicBlock>(*I)) // Don't enumerate BB operand to BlockAddress.
          EnumerateValue(*I);

      // Finally, add the value.  Doing this could make the ValueID reference be
      // dangling, don't reuse it.
      Values.push_back(std::make_pair(V, 1U));
      ValueMap[V] = Values.size();
      return;
    }
  }

  // Add the value.
  Values.push_back(std::make_pair(V, 1U));
  ValueID = Values.size();
}

void ValueEnumerator::EnumerateType(Type *Ty) {
  unsigned *TypeID = &TypeMap[Ty];

  // We've already seen this type.
  if (*TypeID)
    return;

  // If it is a non-anonymous struct, mark the type as being visited so that we
  // don't recursively visit it.  This is safe because we allow forward
  // references of these in the bitcode reader.
  if (StructType *STy = dyn_cast<StructType>(Ty))
    if (!STy->isLiteral())
      *TypeID = ~0U;

  // Enumerate all of the subtypes before we enumerate this type.  This ensures
  // that the type will be enumerated in an order that can be directly built.
  for (Type::subtype_iterator I = Ty->subtype_begin(), E = Ty->subtype_end();
       I != E; ++I)
    EnumerateType(*I);

  // Refresh the TypeID pointer in case the table rehashed.
  TypeID = &TypeMap[Ty];

  // Check to see if we got the pointer another way.  This can happen when
  // enumerating recursive types that hit the base case deeper than they start.
  //
  // If this is actually a struct that we are treating as forward ref'able,
  // then emit the definition now that all of its contents are available.
  if (*TypeID && *TypeID != ~0U)
    return;

  // Add this type now that its contents are all happily enumerated.
  Types.push_back(Ty);

  *TypeID = Types.size();
}

// Enumerate the types for the specified value.  If the value is a constant,
// walk through it, enumerating the types of the constant.
void ValueEnumerator::EnumerateOperandType(const Value *V) {
  EnumerateType(V->getType());

  if (auto *MD = dyn_cast<MetadataAsValue>(V)) {
    assert(!isa<LocalAsMetadata>(MD->getMetadata()) &&
           "Function-local metadata should be left for later");
    EnumerateMetadata(MD->getMetadata());
    return;
  }

  const Constant *C = dyn_cast<Constant>(V);
  if (!C)
    return;
  // If this constant is already enumerated, ignore it, we know its type must
  // be enumerated.
  if (ValueMap.count(C))
    return;

  // This constant may have operands, make sure to enumerate the types in
  // them.
  for (unsigned i = 0, e = C->getNumOperands(); i != e; ++i) {
    const Value *Op = C->getOperand(i);

    // Don't enumerate basic blocks here, this happens as operands to
    // blockaddress.
    if (isa<BasicBlock>(Op))
      continue;

    EnumerateOperandType(Op);
  }
}

void ValueEnumerator::EnumerateAttributes(const AttributeSet &PAL) {
  if (PAL.isEmpty())
    return; // null is always 0.
  // Do a lookup.
  unsigned &Entry = AttributeMap[PAL.getRawPointer()];
  if (Entry == 0) {
    // Never saw this before, add it.
    Attributes.push_back(PAL);
    Entry = Attributes.size();
  }
}

void ValueEnumerator::incorporateFunction(const Function &F) {
  InstructionCount = 0;
  NumModuleValues = Values.size();
  NumModuleMDs = MDs.size();

  // Adding function arguments to the value table.
  for (auto &A : F.args())
    EnumerateValue(&A);

  FirstFuncConstantID = Values.size();

  // Add all function-level constants to the value table.
  for (auto &BB : F) {
    for (auto &I : BB)
      for (auto &O : I.operands()) {
        if ((isa<Constant>(O) && !isa<GlobalValue>(O)) || isa<InlineAsm>(O))
          EnumerateValue(O);
      }
    BasicBlocks.push_back(&BB);
    ValueMap[&BB] = BasicBlocks.size();
  }

  // Optimize the constant layout.
  OptimizeConstants(FirstFuncConstantID, Values.size());

  // Add the function's parameter attributes so they are available for use in
  // the function's instruction.
  EnumerateAttributes(F.getAttributes());

  FirstInstID = Values.size();

  SmallVector<LocalAsMetadata *, 8> FnLocalMDVector;
  // Add all of the instructions.
  for (auto &BB : F) {
    for (auto &I : BB) {
      for (auto &O : I.operands()) {
        if (auto *MD = dyn_cast<llvm::MetadataAsValue>(O))
          if (auto *Local = dyn_cast<LocalAsMetadata>(MD->getMetadata()))
            // Enumerate metadata after the instructions they might refer to.
            FnLocalMDVector.push_back(Local);
      }

      if (!I.getType()->isVoidTy())
        EnumerateValue(&I);
    }
  }

  // Add all of the function-local metadata.
  for (unsigned i = 0, e = FnLocalMDVector.size(); i != e; ++i)
    EnumerateFunctionLocalMetadata(FnLocalMDVector[i]);
}

void ValueEnumerator::purgeFunction() {
  /// Remove purged values from the ValueMap.
  for (unsigned i = NumModuleValues, e = Values.size(); i != e; ++i)
    ValueMap.erase(Values[i].first);

  for (unsigned i = NumModuleMDs, e = MDs.size(); i != e; ++i)
    MDValueMap.erase(MDs[i]);

  for (unsigned i = 0, e = BasicBlocks.size(); i != e; ++i)
    ValueMap.erase(BasicBlocks[i]);

  Values.resize(NumModuleValues);
  MDs.resize(NumModuleMDs);
  BasicBlocks.clear();
  FunctionLocalMDs.clear();
}

static void IncorporateFunctionInfoGlobalBBIDs(
    const Function *F, DenseMap<const BasicBlock *, unsigned> &IDMap) {
  unsigned Counter = 0;
  for (auto &BB : *F)
    IDMap[&BB] = ++Counter;
}

/// getGlobalBasicBlockID - This returns the function-specific ID for the
/// specified basic block.  This is relatively expensive information, so it
/// should only be used by rare constructs such as address-of-label.
unsigned ValueEnumerator::getGlobalBasicBlockID(const BasicBlock *BB) const {
  unsigned &Idx = GlobalBasicBlockIDs[BB];
  if (Idx != 0)
    return Idx - 1;

  IncorporateFunctionInfoGlobalBBIDs(BB->getParent(), GlobalBasicBlockIDs);
  return getGlobalBasicBlockID(BB);
}
