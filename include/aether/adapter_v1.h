#ifndef AETHER_ADAPTER_V1_H_
#define AETHER_ADAPTER_V1_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version 1 is Linux C ABI. EOS opens one verified library per family.
// All strings and JSON buffers are UTF-8; the caller owns input and output
// buffers. The adapter owns its instance until destroy. Functions return 0 on
// success; call writes a bounded error message on failure. The adapter must
// validate its own settings and enforce physical limits and movement leases.
#define AETHER_ADAPTER_ABI_V1 1u
#define AETHER_ADAPTER_CAMERA 1u
#define AETHER_ADAPTER_BASE 2u
#define AETHER_ADAPTER_MANIPULATOR 4u
#define AETHER_ADAPTER_MAX_REPLY_BYTES (64u * 1024u * 1024u)
// Camera capture uses the maximum. Other operations have a 1 MiB output cap.

// call operation IDs: 1 status, 2 health, 3 camera capture, 4 base observe,
// 5 base move, 6 manipulator observe, 7 manipulator move. Inputs and outputs
// use the v2 typed hardware JSON representation. Move input is
// {"meta":CommandMeta,"movement":BaseMovement|ManipulatorMovement}; move
// output is "accepted" | "completed" | "cancelled" | "ambiguous".
// status output is {"state":"starting|ready|stopped|faulted",
// "active_command_id":null|string}; health output is
// {"state":"healthy|degraded|unavailable","code":null|string,
// "message":null|string,"last_ok_monotonic_ns":null|integer}.
typedef struct AetherAdapterV1 {
  uint32_t abi_version;
  const char* family;
  uint32_t kind_mask;
  // node_json is {"path":canonical_path,"kind":camera|base|manipulator,
  // "node":validated_node_profile}.
  void* (*create)(const uint8_t* node_json, size_t node_len);
  int32_t (*call)(void* instance, uint32_t operation, const uint8_t* input, size_t input_len,
                  uint8_t* output, size_t output_capacity, size_t* output_len);
  // stop must be callable concurrently with a blocked call and latch before
  // returning. Physical actuators must have independent bounded stop behavior.
  int32_t (*stop)(void* instance);
  void (*destroy)(void* instance);
} AetherAdapterV1;

// Export exactly this symbol. The returned descriptor must outlive all instances.
const AetherAdapterV1* aether_adapter_v1(void);

#ifdef __cplusplus
}
#endif
#endif  // AETHER_ADAPTER_V1_H_
