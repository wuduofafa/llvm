//===- llvm/unittest/AsmParser/AsmParserTest.cpp - asm parser unittests ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/AsmParser/SlotMapping.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

TEST(AsmParserTest, NullTerminatedInput) {
  LLVMContext &Ctx = getGlobalContext();
  StringRef Source = "; Empty module \n";
  SMDiagnostic Error;
  auto Mod = parseAssemblyString(Source, Error, Ctx);

  EXPECT_TRUE(Mod != nullptr);
  EXPECT_TRUE(Error.getMessage().empty());
}

#ifdef GTEST_HAS_DEATH_TEST
#ifndef NDEBUG

TEST(AsmParserTest, NonNullTerminatedInput) {
  LLVMContext &Ctx = getGlobalContext();
  StringRef Source = "; Empty module \n\1\2";
  SMDiagnostic Error;
  std::unique_ptr<Module> Mod;
  EXPECT_DEATH(Mod = parseAssemblyString(Source.substr(0, Source.size() - 2),
                                         Error, Ctx),
               "Buffer is not null terminated!");
}

#endif
#endif

TEST(AsmParserTest, SlotMappingTest) {
  LLVMContext &Ctx = getGlobalContext();
  StringRef Source = "@0 = global i32 0\n !0 = !{}\n !42 = !{i32 42}";
  SMDiagnostic Error;
  SlotMapping Mapping;
  auto Mod = parseAssemblyString(Source, Error, Ctx, &Mapping);

  EXPECT_TRUE(Mod != nullptr);
  EXPECT_TRUE(Error.getMessage().empty());

  ASSERT_EQ(Mapping.GlobalValues.size(), 1u);
  EXPECT_TRUE(isa<GlobalVariable>(Mapping.GlobalValues[0]));

  EXPECT_EQ(Mapping.MetadataNodes.size(), 2u);
  EXPECT_EQ(Mapping.MetadataNodes.count(0), 1u);
  EXPECT_EQ(Mapping.MetadataNodes.count(42), 1u);
  EXPECT_EQ(Mapping.MetadataNodes.count(1), 0u);
}

TEST(AsmParserTest, TypeAndConstantValueParsing) {
  LLVMContext &Ctx = getGlobalContext();
  SMDiagnostic Error;
  StringRef Source = "define void @test() {\n  entry:\n  ret void\n}";
  auto Mod = parseAssemblyString(Source, Error, Ctx);
  ASSERT_TRUE(Mod != nullptr);
  auto &M = *Mod;

  const Value *V;
  V = parseConstantValue("double 3.5", Error, M);
  ASSERT_TRUE(V);
  EXPECT_TRUE(V->getType()->isDoubleTy());
  ASSERT_TRUE(isa<ConstantFP>(V));
  EXPECT_TRUE(cast<ConstantFP>(V)->isExactlyValue(3.5));

  V = parseConstantValue("i32 42", Error, M);
  ASSERT_TRUE(V);
  EXPECT_TRUE(V->getType()->isIntegerTy());
  ASSERT_TRUE(isa<ConstantInt>(V));
  EXPECT_TRUE(cast<ConstantInt>(V)->equalsInt(42));

  V = parseConstantValue("<4 x i32> <i32 0, i32 1, i32 2, i32 3>", Error, M);
  ASSERT_TRUE(V);
  EXPECT_TRUE(V->getType()->isVectorTy());
  ASSERT_TRUE(isa<ConstantDataVector>(V));

  V = parseConstantValue("i32 add (i32 1, i32 2)", Error, M);
  ASSERT_TRUE(V);
  ASSERT_TRUE(isa<ConstantInt>(V));

  V = parseConstantValue("i8* blockaddress(@test, %entry)", Error, M);
  ASSERT_TRUE(V);
  ASSERT_TRUE(isa<BlockAddress>(V));

  V = parseConstantValue("i8** undef", Error, M);
  ASSERT_TRUE(V);
  ASSERT_TRUE(isa<UndefValue>(V));

  EXPECT_FALSE(parseConstantValue("duble 3.25", Error, M));
  EXPECT_EQ(Error.getMessage(), "expected type");

  EXPECT_FALSE(parseConstantValue("i32 3.25", Error, M));
  EXPECT_EQ(Error.getMessage(), "floating point constant invalid for type");

  EXPECT_FALSE(parseConstantValue("i32* @foo", Error, M));
  EXPECT_EQ(Error.getMessage(), "expected a constant value");

  EXPECT_FALSE(parseConstantValue("i32 3, ", Error, M));
  EXPECT_EQ(Error.getMessage(), "expected end of string");
}

TEST(AsmParserTest, TypeAndConstantValueWithSlotMappingParsing) {
  LLVMContext &Ctx = getGlobalContext();
  SMDiagnostic Error;
  StringRef Source =
      "%st = type { i32, i32 }\n"
      "@v = common global [50 x %st] zeroinitializer, align 16\n"
      "%0 = type { i32, i32, i32, i32 }\n"
      "@g = common global [50 x %0] zeroinitializer, align 16\n"
      "define void @marker4(i64 %d) {\n"
      "entry:\n"
      "  %conv = trunc i64 %d to i32\n"
      "  store i32 %conv, i32* getelementptr inbounds "
      "    ([50 x %st], [50 x %st]* @v, i64 0, i64 0, i32 0), align 16\n"
      "  store i32 %conv, i32* getelementptr inbounds "
      "    ([50 x %0], [50 x %0]* @g, i64 0, i64 0, i32 0), align 16\n"
      "  ret void\n"
      "}";
  SlotMapping Mapping;
  auto Mod = parseAssemblyString(Source, Error, Ctx, &Mapping);
  ASSERT_TRUE(Mod != nullptr);
  auto &M = *Mod;

  const Value *V;
  V = parseConstantValue("i32* getelementptr inbounds ([50 x %st], [50 x %st]* "
                         "@v, i64 0, i64 0, i32 0)",
                         Error, M, &Mapping);
  ASSERT_TRUE(V);
  ASSERT_TRUE(isa<ConstantExpr>(V));

  V = parseConstantValue("i32* getelementptr inbounds ([50 x %0], [50 x %0]* "
                         "@g, i64 0, i64 0, i32 0)",
                         Error, M, &Mapping);
  ASSERT_TRUE(V);
  ASSERT_TRUE(isa<ConstantExpr>(V));
}

TEST(AsmParserTest, TypeWithSlotMappingParsing) {
  LLVMContext &Ctx = getGlobalContext();
  SMDiagnostic Error;
  StringRef Source =
      "%st = type { i32, i32 }\n"
      "@v = common global [50 x %st] zeroinitializer, align 16\n"
      "%0 = type { i32, i32, i32, i32 }\n"
      "@g = common global [50 x %0] zeroinitializer, align 16\n"
      "define void @marker4(i64 %d) {\n"
      "entry:\n"
      "  %conv = trunc i64 %d to i32\n"
      "  store i32 %conv, i32* getelementptr inbounds "
      "    ([50 x %st], [50 x %st]* @v, i64 0, i64 0, i32 0), align 16\n"
      "  store i32 %conv, i32* getelementptr inbounds "
      "    ([50 x %0], [50 x %0]* @g, i64 0, i64 0, i32 0), align 16\n"
      "  ret void\n"
      "}";
  SlotMapping Mapping;
  auto Mod = parseAssemblyString(Source, Error, Ctx, &Mapping);
  ASSERT_TRUE(Mod != nullptr);
  auto &M = *Mod;

  // Check we properly parse integer types.
  Type *Ty;
  Ty = parseType("i32", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isIntegerTy());
  ASSERT_TRUE(Ty->getPrimitiveSizeInBits() == 32);

  // Check we properly parse integer types with exotic size.
  Ty = parseType("i13", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isIntegerTy());
  ASSERT_TRUE(Ty->getPrimitiveSizeInBits() == 13);

  // Check we properly parse floating point types.
  Ty = parseType("float", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isFloatTy());

  Ty = parseType("double", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isDoubleTy());

  // Check we properly parse struct types.
  // Named struct.
  Ty = parseType("%st", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isStructTy());

  // Check the details of the struct.
  StructType *ST = cast<StructType>(Ty);
  ASSERT_TRUE(ST->getNumElements() == 2);
  for (unsigned i = 0, e = ST->getNumElements(); i != e; ++i) {
    Ty = ST->getElementType(i);
    ASSERT_TRUE(Ty->isIntegerTy());
    ASSERT_TRUE(Ty->getPrimitiveSizeInBits() == 32);
  }

  // Anonymous struct.
  Ty = parseType("%0", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isStructTy());

  // Check the details of the struct.
  ST = cast<StructType>(Ty);
  ASSERT_TRUE(ST->getNumElements() == 4);
  for (unsigned i = 0, e = ST->getNumElements(); i != e; ++i) {
    Ty = ST->getElementType(i);
    ASSERT_TRUE(Ty->isIntegerTy());
    ASSERT_TRUE(Ty->getPrimitiveSizeInBits() == 32);
  }

  // Check we properly parse vector types.
  Ty = parseType("<5 x i32>", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isVectorTy());

  // Check the details of the vector.
  VectorType *VT = cast<VectorType>(Ty);
  ASSERT_TRUE(VT->getNumElements() == 5);
  ASSERT_TRUE(VT->getBitWidth() == 160);
  Ty = VT->getElementType();
  ASSERT_TRUE(Ty->isIntegerTy());
  ASSERT_TRUE(Ty->getPrimitiveSizeInBits() == 32);

  // Opaque struct.
  Ty = parseType("%opaque", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isStructTy());

  ST = cast<StructType>(Ty);
  ASSERT_TRUE(ST->isOpaque());

  // Check we properly parse pointer types.
  // One indirection.
  Ty = parseType("i32*", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isPointerTy());

  PointerType *PT = cast<PointerType>(Ty);
  Ty = PT->getElementType();
  ASSERT_TRUE(Ty->isIntegerTy());
  ASSERT_TRUE(Ty->getPrimitiveSizeInBits() == 32);

  // Two indirections.
  Ty = parseType("i32**", Error, M, &Mapping);
  ASSERT_TRUE(Ty);
  ASSERT_TRUE(Ty->isPointerTy());

  PT = cast<PointerType>(Ty);
  Ty = PT->getElementType();
  ASSERT_TRUE(Ty->isPointerTy());

  PT = cast<PointerType>(Ty);
  Ty = PT->getElementType();
  ASSERT_TRUE(Ty->isIntegerTy());
  ASSERT_TRUE(Ty->getPrimitiveSizeInBits() == 32);
}

} // end anonymous namespace
