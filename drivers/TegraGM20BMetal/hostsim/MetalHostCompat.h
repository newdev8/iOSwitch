// The Metal SDK pieces the frontend uses that a macOS SDK before 15.0 lacks. The harness's clients set literal
// strides and the mock never pages, so the shim's protocol declarations only need to type what the frontend calls.
// No SDK header has been read at -include time: freestanding types only (the parameter types of a protocol
// declaration are not checked against the class's, only the selectors are).
#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 140000
#ifndef MetalHostCompat_h
#define MetalHostCompat_h

#include <stdint.h>

typedef signed char BOOL;

#ifdef __OBJC__
@class NSString;

#define MTLBufferLayoutStrideDynamic (~0ULL)
#define MTLLanguageVersion3_1 0x30001
#define MTLTextureUsageShaderAtomic 8

// The mock's clients make no bfloat arguments; the switch cases only need distinct compile-time values.
#define MTLDataTypeBFloat 1000
#define MTLDataTypeBFloat2 1001
#define MTLDataTypeBFloat3 1002
#define MTLDataTypeBFloat4 1003

// The frontend's DMA cache maintenance is the device's AArch64 asm; the mock's DMA does not need it.
#define SWITCH_DCACHE_OP(op, address) ((void)0)
#define SWITCH_DCACHE_BARRIER() ((void)0)

@interface MTLResidencySetDescriptor
@property(copy) NSString *label;
@property uint64_t initialCapacity;
@end

@interface MTLArchitecture
+ (instancetype)alloc;
- (instancetype)initWithCPUType:(int)type cpuSubtype:(int)subtype;
@end

@protocol MTLAllocation
@property(readonly) uint64_t allocatedSize;
@end

@protocol MTLResidencySet
@property(readonly) uint64_t allocatedSize;
@property(readonly) id device;
@property(copy) NSString *label;
- (void)addAllocation:(id)allocation;
- (void)addAllocations:(const id _Nonnull[])allocations count:(uint64_t)count;
- (void)removeAllocation:(id)allocation;
- (void)removeAllocations:(const id _Nonnull[])allocations count:(uint64_t)count;
- (void)removeAllAllocations;
- (BOOL)containsAllocation:(id)allocation;
- (id)allAllocations;
- (uint64_t)allocationCount;
- (void)commit;
- (void)requestResidency;
- (void)endResidency;
@end

#endif
#endif
#endif
