//===-- Bitcode/Writer/ValueEnumerator.h - Number values --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class gives values and types Unique ID's.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BACKPORT_ENUMERATOR_H
#define LLVM_BACKPORT_ENUMERATOR_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DebugInfoMetadata.h"

#include <vector>
namespace llvm {
class Type;
class Value;
class Instruction;
class BasicBlock;
class Function;
class Module;
class Metadata;
class MDNode;
class MDTuple;
class NamedMDNode;
class LocalAsMetadata;
class AttributeSet;
class ValueSymbolTable;
class MDSymbolTable;
class raw_ostream;

namespace ver_3_1 {

class ValueEnumerator {
public:
  typedef std::vector<Type *> TypeList;

  // For each value, we remember its Value* and occurrence frequency.
  typedef std::vector<std::pair<const Value *, unsigned>> ValueList;

private:
  const Module &M;
  LLVMContext &C;

  typedef DenseMap<Type *, unsigned> TypeMapType;
  TypeMapType TypeMap;
  TypeList Types;

  typedef DenseMap<const Value *, unsigned> ValueMapType;
  ValueMapType ValueMap;
  ValueList Values;

  std::vector<const Metadata *> MDs;
  SmallVector<const LocalAsMetadata *, 8> FunctionLocalMDs;
  typedef llvm::DenseMap<const Metadata *, unsigned> MetadataMapType;
  MetadataMapType MDValueMap;
  bool HasMDString;
  bool HasDILocation;
  // Build mapping from debuginfo to LLVM value.
  typedef llvm::DenseMap<const DISubprogram *, const GlobalObject *> DIMapType;
  DIMapType DIMap;

  typedef DenseMap<void *, unsigned> AttributeMapType;
  AttributeMapType AttributeMap;
  std::vector<AttributeSet> Attributes;

  /// GlobalBasicBlockIDs - This map memoizes the basic block ID's referenced by
  /// the "getGlobalBasicBlockID" method.
  mutable DenseMap<const BasicBlock *, unsigned> GlobalBasicBlockIDs;

  typedef DenseMap<const Instruction *, unsigned> InstructionMapType;
  InstructionMapType InstructionMap;
  unsigned InstructionCount;

  /// BasicBlocks - This contains all the basic blocks for the currently
  /// incorporated function.  Their reverse mapping is stored in ValueMap.
  std::vector<const BasicBlock *> BasicBlocks;

  /// When a function is incorporated, this is the size of the Values list
  /// before incorporation.
  unsigned NumModuleValues;

  /// When a function is incorporated, this is the size of the MDValues list
  /// before incorporation.
  unsigned NumModuleMDs;

  unsigned FirstFuncConstantID;
  unsigned FirstInstID;

  ValueEnumerator(const ValueEnumerator &) = delete;
  void operator=(const ValueEnumerator &) = delete;

public:
  ValueEnumerator(const Module *M);

  void dump() const;
  void print(raw_ostream &OS, const ValueMapType &Map, const char *Name) const;
  void print(raw_ostream &OS, const MetadataMapType &Map,
             const char *Name) const;

  unsigned getValueID(const Value *V) const;
  unsigned getMetadataID(const Metadata *MD) const {
    auto ID = getMetadataOrNullID(MD);
    assert(ID != 0 && "Metadata not in slotcalculator!");
    return ID - 1;
  }
  unsigned getMetadataOrNullID(const Metadata *MD) const {
    return MDValueMap.lookup(MD);
  }

  bool hasMDString() const { return HasMDString; }
  bool hasDILocation() const { return HasDILocation; }

  unsigned getTypeID(Type *T) const {
    TypeMapType::const_iterator I = TypeMap.find(T);
    assert(I != TypeMap.end() && "Type not in ValueEnumerator!");
    return I->second - 1;
  }

  unsigned getInstructionID(const Instruction *I) const;
  void setInstructionID(const Instruction *I);

  unsigned getAttributeID(const AttributeSet &PAL) const {
    if (PAL.isEmpty())
      return 0; // Null maps to zero.
    AttributeMapType::const_iterator I = AttributeMap.find(PAL.getRawPointer());
    assert(I != AttributeMap.end() && "Attribute not in ValueEnumerator!");
    return I->second;
  }

  /// getFunctionConstantRange - Return the range of values that corresponds to
  /// function-local constants.
  void getFunctionConstantRange(unsigned &Start, unsigned &End) const {
    Start = FirstFuncConstantID;
    End = FirstInstID;
  }

  const ValueList &getValues() const { return Values; }
  const std::vector<const llvm::Metadata *> &getMDs() const { return MDs; }
  const SmallVectorImpl<const LocalAsMetadata *> &
  getFunctionLocalMDValues() const {
    return FunctionLocalMDs;
  }
  const TypeList &getTypes() const { return Types; }
  const std::vector<const BasicBlock *> &getBasicBlocks() const {
    return BasicBlocks;
  }
  const std::vector<AttributeSet> &getAttributes() const { return Attributes; }

  /// getGlobalBasicBlockID - This returns the function-specific ID for the
  /// specified basic block.  This is relatively expensive information, so it
  /// should only be used by rare constructs such as address-of-label.
  unsigned getGlobalBasicBlockID(const BasicBlock *BB) const;

  /// incorporateFunction/purgeFunction - If you'd like to deal with a function,
  /// use these two methods to get its data into the ValueEnumerator!
  ///
  void incorporateFunction(const Function &F);
  void purgeFunction();

  const Function *getFunction(const DISubprogram *DI) const {
    return cast<Function>(DIMap.lookup(DI));
  }

private:
  void OptimizeConstants(unsigned CstStart, unsigned CstEnd);
  void MapDebugInfo(const Function *F);

  void EnumerateInt(uint64_t Val, unsigned NumBits);
  void EnumerateInt32(uint32_t Val);
  void EnumerateInt64(uint64_t Val);
  void EnumerateBool(bool b);
  void EnumerateDwarfTag(unsigned Tag);
  void EnumerateMDString(StringRef S);

  void EnumerateDICompositeTypeBase(const DIType *MD,
                                    const Metadata *ParentType,
                                    const Metadata *MemberTypes,
                                    uint32_t RuntimeLanguage);

  void EnumerateMDTuple(const MDTuple *MD);
  void EnumerateDILocation(const DILocation *MD);
  void EnumerateDIExpression(const DIExpression *MD);
  void EnumerateGenericDINode(const GenericDINode *MD);
  void EnumerateDISubrange(const DISubrange *MD);
  void EnumerateDIEnumerator(const DIEnumerator *MD);
  void EnumerateDIBasicType(const DIBasicType *MD);
  void EnumerateDIDerivedType(const DIDerivedType *MD);
  void EnumerateDICompositeType(const DICompositeType *MD);
  void EnumerateDISubroutineType(const DISubroutineType *MD);
  void EnumerateDIFile(const DIFile *MD);
  void EnumerateDICompileUnit(const DICompileUnit *MD);
  void EnumerateDISubprogram(const DISubprogram *MD);
  void EnumerateDILexicalBlock(const DILexicalBlock *MD);
  void EnumerateDILexicalBlockFile(const DILexicalBlockFile *MD);
  void EnumerateDINamespace(const DINamespace *MD);
  void EnumerateDIModule(const DIModule *MD);
  void EnumerateDITemplateTypeParameter(const DITemplateTypeParameter *MD);
  void EnumerateDITemplateValueParameter(const DITemplateValueParameter *MD);
  void EnumerateDIGlobalVariable(const DIGlobalVariable *MD);
  void EnumerateDILocalVariable(const DILocalVariable *MD);
  void EnumerateDIObjCProperty(const DIObjCProperty *MD);
  void EnumerateDIImportedEntity(const DIImportedEntity *MD);
  void EnumerateDIMacro(const DIMacro *MD);
  void EnumerateDIMacroFile(const DIMacroFile *MD);

  void EnumerateMDNodeOperands(const MDNode *N);
  void EnumerateMDNode(const MDNode *MD);
  void EnumerateMetadata(const Metadata *MD);
  void EnumerateFunctionLocalMetadata(const LocalAsMetadata *Local);
  void EnumerateNamedMDNode(const NamedMDNode *NMD);
  void EnumerateValue(const Value *V);
  void EnumerateType(Type *T);
  void EnumerateOperandType(const Value *V);
  void EnumerateAttributes(const AttributeSet &PAL);

  void EnumerateValueSymbolTable(const ValueSymbolTable &ST);
  void EnumerateNamedMetadata(const Module *M);
};
}
} // End llvm namespace

#endif
