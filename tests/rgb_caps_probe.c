/* Capability evidence, not proof that an imported image encodes correctly. */
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <vulkan/vulkan.h>

static bool extension(const VkExtensionProperties *properties, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(properties[i].extensionName, name))
            return true;
    return false;
}

static int formats(VkPhysicalDevice device, PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR query,
                   const VkVideoProfileInfoKHR *profile)
{
    VkVideoProfileListInfoKHR profiles = {.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
                                          .profileCount = 1,
                                          .pProfiles = profile};
    for (unsigned kind = 0; kind < 2; ++kind) {
        VkPhysicalDeviceVideoFormatInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
            .pNext = &profiles,
            .imageUsage = kind ? VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR
                               : VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR};
        uint32_t count = 0;
        VkResult result = query(device, &info, &count, nullptr);
        if (result != VK_SUCCESS || !count)
            return -1;
        VkVideoFormatPropertiesKHR *properties = calloc(count, sizeof(*properties));
        if (!properties)
            return -1;
        for (uint32_t i = 0; i < count; ++i)
            properties[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        result = query(device, &info, &count, properties);
        if (result == VK_SUCCESS)
            for (uint32_t i = 0; i < count; ++i)
                printf("video-format kind=%s format=%d tiling=%d usage=%#x\n",
                       kind ? "reconstruction" : "source", properties[i].format,
                       properties[i].imageTiling, properties[i].imageUsageFlags);
        free(properties);
        if (result != VK_SUCCESS)
            return -1;
    }
    return 0;
}

static int modifiers(VkPhysicalDevice device, const VkVideoProfileInfoKHR *profile, VkFormat format)
{
    VkDrmFormatModifierPropertiesList2EXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
    VkFormatProperties2 properties = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                      .pNext = &list};
    vkGetPhysicalDeviceFormatProperties2(device, format, &properties);
    uint32_t capacity = list.drmFormatModifierCount;
    if (!capacity)
        return -1;
    list.pDrmFormatModifierProperties =
        calloc(capacity, sizeof(*list.pDrmFormatModifierProperties));
    if (!list.pDrmFormatModifierProperties)
        return -1;
    vkGetPhysicalDeviceFormatProperties2(device, format, &properties);
    if (list.drmFormatModifierCount > capacity) {
        free(list.pDrmFormatModifierProperties);
        return -1;
    }
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        const VkDrmFormatModifierProperties2EXT *p = &list.pDrmFormatModifierProperties[i];
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = p->drmFormatModifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkVideoProfileListInfoKHR profiles = {.sType =
                                                  VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
                                              .pNext = &modifier,
                                              .profileCount = 1,
                                              .pProfiles = profile};
        VkPhysicalDeviceExternalImageFormatInfo external = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &profiles,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &external,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR};
        VkExternalImageFormatProperties ext = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 output = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                           .pNext = &ext};
        VkResult result = vkGetPhysicalDeviceImageFormatProperties2(device, &info, &output);
        printf("modifier format=%d value=%#018llx planes=%u features=%#llx result=%d external=%#x "
               "max=%ux%u\n",
               format, (unsigned long long)p->drmFormatModifier, p->drmFormatModifierPlaneCount,
               (unsigned long long)p->drmFormatModifierTilingFeatures, result,
               ext.externalMemoryProperties.externalMemoryFeatures,
               output.imageFormatProperties.maxExtent.width,
               output.imageFormatProperties.maxExtent.height);
    }
    free(list.pDrmFormatModifierProperties);
    return 0;
}

static int vulkan(dev_t wanted)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "gpuKey RGB capability probe",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app};
    VkInstance instance;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        return -1;
    int status = -1;
    uint32_t count = 0;
    VkPhysicalDevice *devices = nullptr;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || !count)
        goto out;
    devices = calloc(count, sizeof(*devices));
    if (!devices || vkEnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS)
        goto out;
    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR query_caps =
        (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR query_formats =
        (PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
    if (!query_caps || !query_formats)
        goto out;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t n = 0;
        if (vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &n, nullptr) != VK_SUCCESS)
            continue;
        VkExtensionProperties *extensions = calloc(n, sizeof(*extensions));
        if (!extensions)
            goto out;
        if (vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &n, extensions) !=
            VK_SUCCESS) {
            free(extensions);
            goto out;
        }
        bool drm = extension(extensions, n, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        bool rgb = extension(extensions, n, VK_VALVE_VIDEO_ENCODE_RGB_CONVERSION_EXTENSION_NAME);
        bool av1 = extension(extensions, n, VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME);
        free(extensions);
        if (!drm)
            continue;
        VkPhysicalDeviceDriverProperties driver = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceDrmPropertiesEXT node = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT, .pNext = &driver};
        VkPhysicalDeviceProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &node};
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        if (!node.hasRender || node.renderMajor != major(wanted) ||
            node.renderMinor != minor(wanted))
            continue;
        printf("vulkan device=%s driver=%s %s rgb=%u av1=%u\n", props.properties.deviceName,
               driver.driverName, driver.driverInfo, (unsigned)rgb, (unsigned)av1);
        if (!rgb || !av1)
            goto out;
        VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE feature = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_RGB_CONVERSION_FEATURES_VALVE};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                              .pNext = &feature};
        vkGetPhysicalDeviceFeatures2(devices[i], &features);
        printf("rgb-feature=%u\n", feature.videoEncodeRgbConversion);
        for (unsigned bits = 8; bits <= 10; bits += 2) {
            VkVideoEncodeProfileRgbConversionInfoVALVE conversion = {
                .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_PROFILE_RGB_CONVERSION_INFO_VALVE,
                .performEncodeRgbConversion = VK_TRUE};
            VkVideoEncodeAV1ProfileInfoKHR codec = {
                .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,
                .pNext = &conversion,
                .stdProfile = STD_VIDEO_AV1_PROFILE_MAIN};
            VkVideoProfileInfoKHR profile = {
                .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
                .pNext = &codec,
                .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
                .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
                .lumaBitDepth = bits == 8 ? VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR
                                          : VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
                .chromaBitDepth = bits == 8 ? VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR
                                            : VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR};
            VkVideoEncodeRgbConversionCapabilitiesVALVE colors = {
                .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RGB_CONVERSION_CAPABILITIES_VALVE};
            VkVideoEncodeAV1CapabilitiesKHR av1_caps = {
                .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR, .pNext = &colors};
            VkVideoEncodeCapabilitiesKHR encode = {
                .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, .pNext = &av1_caps};
            VkVideoCapabilitiesKHR caps = {.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
                                           .pNext = &encode};
            VkResult result = query_caps(devices[i], &profile, &caps);
            printf("av1-rgb bits=%u result=%d models=%#x ranges=%#x x=%#x y=%#x min=%ux%u "
                   "max=%ux%u dpb=%u refs=%u\n",
                   bits, result, colors.rgbModels, colors.rgbRanges, colors.xChromaOffsets,
                   colors.yChromaOffsets, caps.minCodedExtent.width, caps.minCodedExtent.height,
                   caps.maxCodedExtent.width, caps.maxCodedExtent.height, caps.maxDpbSlots,
                   caps.maxActiveReferencePictures);
            if (result != VK_SUCCESS || formats(devices[i], query_formats, &profile) < 0 ||
                modifiers(devices[i], &profile,
                          bits == 8 ? VK_FORMAT_B8G8R8A8_UNORM
                                    : VK_FORMAT_A2R10G10B10_UNORM_PACK32) < 0)
                goto out;
        }
        status = 0;
        break;
    }
out:
    free(devices);
    vkDestroyInstance(instance, nullptr);
    return status;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    struct stat st;
    if (fd < 0)
        return 1;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return 1;
    }
    VADisplay display = vaGetDisplayDRM(fd);
    int major_version, minor_version;
    if (!display || vaInitialize(display, &major_version, &minor_version) != VA_STATUS_SUCCESS) {
        close(fd);
        return 1;
    }
    VAConfigAttrib attrs[] = {{.type = VAConfigAttribRTFormat},
                              {.type = VAConfigAttribRateControl}};
    VAStatus result =
        vaGetConfigAttributes(display, VAProfileAV1Profile0, VAEntrypointEncSlice, attrs, 2);
    printf("va device=%s driver=%s result=%d rt-formats=%#x rate-control=%#x\n", path,
           vaQueryVendorString(display), result, attrs[0].value, attrs[1].value);
    vaTerminate(display);
    close(fd);
    return vulkan(st.st_rdev) == 0 ? 0 : 1;
}
