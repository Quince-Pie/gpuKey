# Diagnostic tool only. Preserve RGB profile data and the query-chain case
# explicitly described by VUID-VkQueryPoolCreateInfo-pNext-10918.
{ vulkan-validation-layers }:
vulkan-validation-layers.overrideAttrs (old: {
  pname = "vulkan-validation-layers-rgb";
  patches = (old.patches or [ ]) ++ [ ./vulkan-validation-rgb.patch ];
})
