#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include <bits/stdint-intn.h>

using namespace mlir;

// To avoid name mangling, these are defined in the mini-runtime file.
static constexpr const char *oecTeardownName = "oecTeardown";
static constexpr const char *oecModuleLoadName = "oecModuleLoad";
static constexpr const char *oecModuleGetFunctionName = "oecModuleGetFunction";
static constexpr const char *oecLaunchKernelName = "oecLaunchKernel";
static constexpr const char *oecStreamSynchronizeName = "oecStreamSynchronize";
static constexpr const char *oecStoreParameterName = "oecStoreParameter";
static constexpr const char *oecLoadParametersName = "oecLoadParameters";
static constexpr const char *oecAllocTemporaryName = "oecAllocTemporary";

static constexpr const char *kCubinAnnotation = "nvvm.cubin";
static constexpr const char *kCubinStorageSuffix = "_cubin_cst";

static constexpr const char *kSetupName = "setup";
static constexpr const char *kRunName = "run";
static constexpr const char *kTeardownName = "teardown";

namespace {

class LaunchFuncToCUDACallsPass : public ModulePass<LaunchFuncToCUDACallsPass> {
private:
  LLVM::LLVMDialect *getLLVMDialect() { return llvmDialect; }

  llvm::LLVMContext &getLLVMContext() {
    return getLLVMDialect()->getLLVMContext();
  }

  void initializeCachedTypes() {
    const llvm::Module &module = llvmDialect->getLLVMModule();
    llvmVoidType = LLVM::LLVMType::getVoidTy(llvmDialect);
    llvmPointerType = LLVM::LLVMType::getInt8PtrTy(llvmDialect);
    llvmPointerPointerType = llvmPointerType.getPointerTo();
    llvmInt8Type = LLVM::LLVMType::getInt8Ty(llvmDialect);
    llvmInt32Type = LLVM::LLVMType::getInt32Ty(llvmDialect);
    llvmInt64Type = LLVM::LLVMType::getInt64Ty(llvmDialect);
    llvmIntPtrType = LLVM::LLVMType::getIntNTy(
        llvmDialect, module.getDataLayout().getPointerSizeInBits());
  }

  LLVM::LLVMType getVoidType() { return llvmVoidType; }
  LLVM::LLVMType getPointerType() { return llvmPointerType; }
  LLVM::LLVMType getPointerPointerType() { return llvmPointerPointerType; }
  LLVM::LLVMType getInt8Type() { return llvmInt8Type; }
  LLVM::LLVMType getInt32Type() { return llvmInt32Type; }
  LLVM::LLVMType getInt64Type() { return llvmInt64Type; }
  LLVM::LLVMType getIntPtrType() {
    const llvm::Module &module = getLLVMDialect()->getLLVMModule();
    return LLVM::LLVMType::getIntNTy(
        getLLVMDialect(), module.getDataLayout().getPointerSizeInBits());
  }

  LLVM::LLVMType getCUResultType() {
    // This is declared as an enum in CUDA but helpers use i32.
    return getInt32Type();
  }

  void declareRTFunctions(Location loc);
  Value declareGlobalKernelName(StringRef name, StringRef data, Location loc,
                                OpBuilder &builder);
  LLVM::GlobalOp declareGlobalFuncPtr(StringRef name, Location loc,
                                      OpBuilder &builder);
  LogicalResult declareSetupFunc(LLVM::LLVMFuncOp parentOp, Location loc,
                                 OpBuilder &builder);
  LogicalResult declareTeardownFunc(Location loc, OpBuilder &builder);
  LogicalResult declareLaunchFunc(LLVM::LLVMFuncOp parentOp, Location loc,
                                  OpBuilder &builder);

public:
  // Run the dialect converter on the module.
  void runOnModule() override {
    // Cache the LLVMDialect for the current module.
    llvmDialect = getContext().getRegisteredDialect<LLVM::LLVMDialect>();
    // Cache the used LLVM types.
    initializeCachedTypes();

    // Collect all kernel functions
    SmallVector<LLVM::LLVMFuncOp, 1> parentOps;
    getModule().walk([&](mlir::gpu::LaunchFuncOp op) {
      parentOps.push_back(op.getParentOfType<LLVM::LLVMFuncOp>());
    });
    if (llvm::count(parentOps, parentOps.back()) != parentOps.size()) {
      getModule().emitOpError("expected exactly one kernel function");
      return signalPassFailure();
    }

    // Compute init method
    auto parentOp = parentOps.back();
    OpBuilder builder(parentOp);
    Location loc = parentOp.getLoc();

    // Declare CUDA runtime functions
    declareRTFunctions(loc);

    // Declare the setup and teardown functions
    if (failed(declareSetupFunc(parentOp, loc, builder)))
      return signalPassFailure();
    if (failed(declareTeardownFunc(loc, builder)))
      return signalPassFailure();

    // Declare the kernel function
    if (failed(declareLaunchFunc(parentOp, loc, builder)))
      return signalPassFailure();

    // GPU kernel modules are no longer necessary since we have a global
    // constant with the CUBIN data.
    for (auto m :
         llvm::make_early_inc_range(getModule().getOps<gpu::GPUModuleOp>()))
      m.erase();

    parentOp.erase();
  }

private:
  LLVM::LLVMDialect *llvmDialect;
  LLVM::LLVMType llvmVoidType;
  LLVM::LLVMType llvmPointerType;
  LLVM::LLVMType llvmPointerPointerType;
  LLVM::LLVMType llvmInt8Type;
  LLVM::LLVMType llvmInt32Type;
  LLVM::LLVMType llvmInt64Type;
  LLVM::LLVMType llvmIntPtrType;
};

} // anonymous namespace

// Adds declarations for the needed helper functions from the CUDA wrapper.
// The types in comments give the actual types expected/returned but the API
// uses void pointers. This is fine as they have the same linkage in C.
void LaunchFuncToCUDACallsPass::declareRTFunctions(Location loc) {
  ModuleOp module = getModule();
  OpBuilder builder(module.getBody()->getTerminator());
  if (!module.lookupSymbol(oecTeardownName)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecTeardownName,
        LLVM::LLVMType::getFunctionTy(getCUResultType(), /*isVarArg=*/false));
  }
  if (!module.lookupSymbol(oecModuleLoadName)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecModuleLoadName,
        LLVM::LLVMType::getFunctionTy(
            getCUResultType(),
            {
                getPointerPointerType(), /* CUmodule *module */
                getPointerType()         /* void *cubin */
            },
            /*isVarArg=*/false));
  }
  if (!module.lookupSymbol(oecModuleGetFunctionName)) {
    // The helper uses void* instead of CUDA's opaque CUmodule and
    // CUfunction.
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecModuleGetFunctionName,
        LLVM::LLVMType::getFunctionTy(
            getCUResultType(),
            {
                getPointerPointerType(), /* void **function */
                getPointerType(),        /* void *module */
                getPointerType()         /* char *name */
            },
            /*isVarArg=*/false));
  }
  if (!module.lookupSymbol(oecLaunchKernelName)) {
    // Other than the CUDA api, the wrappers use uintptr_t to match the
    // LLVM type if MLIR's index type, which the GPU dialect uses.
    // Furthermore, they use void* instead of CUDA's opaque CUfunction and
    // CUstream.
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecLaunchKernelName,
        LLVM::LLVMType::getFunctionTy(
            getCUResultType(),
            {
                getPointerType(),       /* void* f */
                getIntPtrType(),        /* intptr_t gridXDim */
                getIntPtrType(),        /* intptr_t gridyDim */
                getIntPtrType(),        /* intptr_t gridZDim */
                getIntPtrType(),        /* intptr_t blockXDim */
                getIntPtrType(),        /* intptr_t blockYDim */
                getIntPtrType(),        /* intptr_t blockZDim */
                getPointerPointerType() /* void **kernelParams */
            },
            /*isVarArg=*/false));
  }
  if (!module.lookupSymbol(oecStreamSynchronizeName)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecStreamSynchronizeName,
        LLVM::LLVMType::getFunctionTy(getCUResultType(), /*isVarArg=*/false));
  }
  if (!module.lookupSymbol(oecStoreParameterName)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecStoreParameterName,
        LLVM::LLVMType::getFunctionTy(
            getCUResultType(),
            {
                getPointerType(), /* void *ptr */
                getInt64Type(),   /* int64 sizeBytes*/
                getInt32Type()    /* int32 device of host */
            },
            /*isVarArg=*/false));
  }
  if (!module.lookupSymbol(oecLoadParametersName)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecLoadParametersName,
        LLVM::LLVMType::getFunctionTy(
            getVoidType(),
            {
                getPointerPointerType(), /* void **ptr */
                getInt32Type(),          /* int32 offset */
                getInt32Type()           /* int32 size */
            },
            /*isVarArg=*/false));
  }
  if (!module.lookupSymbol(oecAllocTemporaryName)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, oecAllocTemporaryName,
        LLVM::LLVMType::getFunctionTy(getPointerType(),
                                      {
                                          getInt64Type() /* int64 size */
                                      },
                                      /*isVarArg=*/false));
  }
}

Value LaunchFuncToCUDACallsPass::declareGlobalKernelName(StringRef name,
                                                         StringRef data,
                                                         Location loc,
                                                         OpBuilder &builder) {
  // Make sure the trailing zero is included in the constant.
  std::vector<char> kernelName(data.begin(), data.end());
  kernelName.push_back('\0');

  std::string globalName = llvm::formatv("{0}_name", name);
  return LLVM::createGlobalString(
      loc, builder, globalName, StringRef(kernelName.data(), kernelName.size()),
      LLVM::Linkage::Internal, llvmDialect);
}

LLVM::GlobalOp
LaunchFuncToCUDACallsPass::declareGlobalFuncPtr(StringRef name, Location loc,
                                                OpBuilder &builder) {
  // Insert at the end
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPoint(getModule().getBody()->getTerminator());

  // Create global variable to store the function handle
  std::string globalName = llvm::formatv("{0}_function", name);
  auto globalOp =
      dyn_cast_or_null<LLVM::GlobalOp>(getModule().lookupSymbol(globalName));
  if (!globalOp)
    return builder.create<LLVM::GlobalOp>(
        loc, getPointerType(),
        /*isConstant=*/false, LLVM::Linkage::Internal, globalName,
        builder.getZeroAttr(getPointerType()));
  return globalOp;
}

// Add the definition of the setup and teardown functions
LogicalResult
LaunchFuncToCUDACallsPass::declareTeardownFunc(Location loc,
                                               OpBuilder &builder) {
  // Insert at the end of the module
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPoint(getModule().getBody()->getTerminator());

  // Verify the method does not conflict with an existing one
  if (getModule().lookupSymbol(kTeardownName))
    return failure();

  // Generate the teardown function
  auto funcOp = builder.create<LLVM::LLVMFuncOp>(
      loc, kTeardownName,
      LLVM::LLVMType::getFunctionTy(getVoidType(), /*isVarArg=*/false));

  auto entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToEnd(entryBlock);

  // Call the teardown method of the oec runtime
  auto teardownFunc =
      getModule().lookupSymbol<LLVM::LLVMFuncOp>(oecTeardownName);
  builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getInt32Type()},
                               builder.getSymbolRefAttr(teardownFunc),
                               ArrayRef<Value>{});

  builder.create<LLVM::ReturnOp>(loc, ValueRange());
  return success();
}

LogicalResult
LaunchFuncToCUDACallsPass::declareSetupFunc(LLVM::LLVMFuncOp parentOp,
                                            Location loc, OpBuilder &builder) {
  // Insert at the end of the module
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPoint(getModule().getBody()->getTerminator());

  // Verify the method does not conflict with an existing one
  std::string setupName =
      llvm::formatv("{0}_{1}", kSetupName, parentOp.getName());
  if (getModule().lookupSymbol(setupName))
    return failure();

  // Clone the kernel launch method
  auto funcOp =
      builder.create<LLVM::LLVMFuncOp>(loc, setupName, parentOp.getType());
  BlockAndValueMapping mapper;
  parentOp.getBody().cloneInto(&funcOp.getBody(), mapper);

  // Walk the cloned op and replace all kernel launch
  SmallVector<mlir::gpu::LaunchFuncOp, 10> launchOps;
  funcOp.walk([&](mlir::gpu::LaunchFuncOp launchOp) {
    // Set the insertion point
    builder.setInsertionPoint(launchOp);

    // Create an LLVM global with CUBIN extracted from the kernel annotation and
    // obtain a pointer to the first byte in it.
    auto kernelModule = getModule().lookupSymbol<gpu::GPUModuleOp>(
        launchOp.getKernelModuleName());
    assert(kernelModule && "expected a kernel module");
    auto cubinAttr = kernelModule.getAttrOfType<StringAttr>(kCubinAnnotation);
    assert(cubinAttr && "missing cubin annotation attribute");
    SmallString<128> nameBuffer(kernelModule.getName());
    nameBuffer.append(kCubinStorageSuffix);
    Value data = LLVM::createGlobalString(
        loc, builder, nameBuffer.str(), cubinAttr.getValue(),
        LLVM::Linkage::Internal, getLLVMDialect());

    // Load the module and compiler the kernel function
    auto one = builder.create<LLVM::ConstantOp>(loc, getInt32Type(),
                                                builder.getI32IntegerAttr(1));
    auto modulePtr =
        builder.create<LLVM::AllocaOp>(loc, getPointerPointerType(), one,
                                       /*alignment=*/0);
    auto moduleLoadFunc =
        getModule().lookupSymbol<LLVM::LLVMFuncOp>(oecModuleLoadName);
    builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getCUResultType()},
                                 builder.getSymbolRefAttr(moduleLoadFunc),
                                 ArrayRef<Value>{modulePtr, data});

    // Get the function from the module. The name corresponds to the name of
    // the kernel function.
    auto funcHandle =
        declareGlobalFuncPtr(launchOp.getKernelModuleName(), loc, builder);
    auto moduleRef =
        builder.create<LLVM::LoadOp>(loc, getPointerType(), modulePtr);
    Value funcPtr = builder.create<LLVM::AddressOfOp>(loc, funcHandle);
    auto kernelName = declareGlobalKernelName(launchOp.getKernelModuleName(),
                                              launchOp.kernel(), loc, builder);
    auto moduleGetFunc =
        getModule().lookupSymbol<LLVM::LLVMFuncOp>(oecModuleGetFunctionName);
    builder.create<LLVM::CallOp>(
        loc, ArrayRef<Type>{getCUResultType()},
        builder.getSymbolRefAttr(moduleGetFunc),
        ArrayRef<Value>{funcPtr, moduleRef, kernelName});

    // Store the launch arguments
    int numKernelOperands = launchOp.getNumKernelOperands();
    for (unsigned idx = 0; idx < numKernelOperands; ++idx) {
      // Get the operand
      auto operand = launchOp.getKernelOperand(idx);
      auto llvmType = operand.getType().cast<LLVM::LLVMType>();
      Value memLocation = builder.create<LLVM::AllocaOp>(
          loc, llvmType.getPointerTo(), one, /*alignment=*/1);
      builder.create<LLVM::StoreOp>(loc, operand, memLocation);
      auto casted =
          builder.create<LLVM::BitcastOp>(loc, getPointerType(), memLocation);

      // Store only struct types on the device and pass scalars by value
      auto device = builder.create<LLVM::ConstantOp>(
          loc, getInt32Type(),
          builder.getI32IntegerAttr(llvmType.isStructTy() ? 1 : 0));

      // Compute the size of the memref
      auto nullPtr = builder.create<LLVM::NullOp>(loc, llvmType.getPointerTo());
      auto gep = builder.create<LLVM::GEPOp>(loc, llvmType.getPointerTo(),
                                             ArrayRef<Value>{nullPtr, one});
      auto size = builder.create<LLVM::PtrToIntOp>(loc, getInt64Type(), gep);
      auto storeFunc =
          getModule().lookupSymbol<LLVM::LLVMFuncOp>(oecStoreParameterName);
      builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getCUResultType()},
                                   builder.getSymbolRefAttr(storeFunc),
                                   ArrayRef<Value>{casted, size, device});
    }

    launchOps.push_back(launchOp);
  });
  // Erase all the launch operations
  for (auto op : launchOps) {
    op.erase();
  }

  // Change the malloc and free calls
  SmallVector<LLVM::CallOp, 10> callOps;
  funcOp.walk([&](LLVM::CallOp callOp) {
    // Set the insertion point
    builder.setInsertionPoint(callOp);

    // Replace all malloc calls with device allocations
    if (callOp.callee().getValueOr("") == "malloc") {
      auto allocFunc =
          getModule().lookupSymbol<LLVM::LLVMFuncOp>(oecAllocTemporaryName);
      auto temporary =
          builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getPointerType()},
                                       builder.getSymbolRefAttr(allocFunc),
                                       ArrayRef<Value>{callOp.getOperand(0)});
      callOp.getResult(0).replaceAllUsesWith(temporary.getResult(0));
      callOps.push_back(callOp);
    }

    // Erase all free calls
    if (callOp.callee().getValueOr("") == "free") {
      callOps.push_back(callOp);
    }
  });
  // Erase all unused callops
  for (auto op : callOps) {
    op.erase();
  }

  return success();
}

LogicalResult
LaunchFuncToCUDACallsPass::declareLaunchFunc(LLVM::LLVMFuncOp parentOp,
                                             Location loc, OpBuilder &builder) {
  // Insert at the end of the module
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPoint(getModule().getBody()->getTerminator());

  // Verify the method does not conflict with an existing one
  std::string runName = llvm::formatv("{0}_{1}", kRunName, parentOp.getName());
  if (getModule().lookupSymbol(runName))
    return failure();

  auto funcOp = builder.create<LLVM::LLVMFuncOp>(
      loc, runName, LLVM::LLVMType::getFunctionTy(getVoidType(), {}));

  auto entryBlock = funcOp.addEntryBlock();
  builder.setInsertionPointToStart(entryBlock);

  // Launch all kernels

  int32_t kernelOperandOffset = 0;
  parentOp.walk([&](mlir::gpu::LaunchFuncOp launchOp) {
    // Load the function
    std::string funcName =
        llvm::formatv("{0}_function", launchOp.getKernelModuleName());
    Value funcPtr = builder.create<LLVM::AddressOfOp>(
        loc, cast<LLVM::GlobalOp>(getModule().lookupSymbol(funcName)));
    auto function =
        builder.create<LLVM::LoadOp>(loc, getPointerType(), funcPtr);

    // Load the kernel function handle
    auto module = getModule();
    auto launchFunc =
        module.lookupSymbol<LLVM::LLVMFuncOp>(oecLaunchKernelName);

    // Setup the parameter array
    auto numKernelOperands = launchOp.getNumKernelOperands();
    auto arraySize = builder.create<LLVM::ConstantOp>(
        loc, getInt32Type(), builder.getI32IntegerAttr(numKernelOperands));
    auto arrayOffset = builder.create<LLVM::ConstantOp>(
        loc, getInt32Type(), builder.getI32IntegerAttr(kernelOperandOffset));
    auto allocOp = builder.create<LLVM::AllocaOp>(loc, getPointerPointerType(),
                                                  arraySize, /*alignment=*/0);
    auto array = allocOp.getResult();
    auto fillFunc =
        module.lookupSymbol<LLVM::LLVMFuncOp>(oecLoadParametersName);
    builder.create<LLVM::CallOp>(
        loc, ArrayRef<Type>{getCUResultType()},
        builder.getSymbolRefAttr(fillFunc),
        ArrayRef<Value>{array, arrayOffset, arraySize});

    // Clone the launch configuration
    // (assuming a constant launch configuration)
    auto gridX = cast<LLVM::ConstantOp>(
        builder.clone(*launchOp.getOperand(0).getDefiningOp()));
    auto gridY = cast<LLVM::ConstantOp>(
        builder.clone(*launchOp.getOperand(1).getDefiningOp()));
    auto gridZ = cast<LLVM::ConstantOp>(
        builder.clone(*launchOp.getOperand(2).getDefiningOp()));
    auto blockX = cast<LLVM::ConstantOp>(
        builder.clone(*launchOp.getOperand(3).getDefiningOp()));
    auto blockY = cast<LLVM::ConstantOp>(
        builder.clone(*launchOp.getOperand(4).getDefiningOp()));
    auto blockZ = cast<LLVM::ConstantOp>(
        builder.clone(*launchOp.getOperand(5).getDefiningOp()));

    // Launch the kernel
    builder.create<LLVM::CallOp>(
        loc, ArrayRef<Type>{getCUResultType()},
        builder.getSymbolRefAttr(launchFunc),
        ArrayRef<Value>{function.getResult(), gridX.getResult(),
                        gridY.getResult(), gridZ.getResult(),
                        blockX.getResult(), blockY.getResult(),
                        blockZ.getResult(), array});

    // Update operand offset
    kernelOperandOffset += numKernelOperands;
  });

  // Sync on the stream
  auto syncFunc =
      getModule().lookupSymbol<LLVM::LLVMFuncOp>(oecStreamSynchronizeName);
  builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getCUResultType()},
                               builder.getSymbolRefAttr(syncFunc),
                               ArrayRef<Value>{});
                               
  // Add a terminator
  builder.create<LLVM::ReturnOp>(loc, ValueRange());
  return success();
}

static PassRegistration<LaunchFuncToCUDACallsPass>
    pass("stencil-gpu-to-cuda",
         "Convert all kernel launches to CUDA runtime calls");