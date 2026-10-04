/*
FDM_INJECTION_STUB.C

An OpenXR API layer that does nothing, named as the Steam Frame's
XR_APILAYER_VALVE_fdm_injection names its library
(libVkLayer_VALVE_fdm_injection.so). The Frame lists that layer as implicit
for every OpenXR app (/vendor/etc/openxr/1/api_layers/implicit.d), but an
Android app (in Lepton) cannot load the system's library from its own
namespace, and the Khronos loader then fails xrCreateInstance altogether;
nor does the Android loader read the variable that would turn the layer
off. With this one in the app's libraries the loader finds a layer of that
name, which hands every call on to the next. (The real layer adds fragment
density maps to Vulkan apps; this game draws with OpenGL ES.) On a Quest no
manifest names it and it is never loaded.
*/

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <stddef.h>

static PFN_xrGetInstanceProcAddr next_get_instance_proc_addr;

static XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(XrInstance instance, const char *name,
	PFN_xrVoidFunction *function)
{
	if (!next_get_instance_proc_addr)
		return XR_ERROR_HANDLE_INVALID;
	return next_get_instance_proc_addr(instance, name, function);
}

static XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(const XrInstanceCreateInfo *info,
	const XrApiLayerCreateInfo *layer_info, XrInstance *instance)
{
	XrApiLayerCreateInfo next_layer_info;

	if (!layer_info || !layer_info->nextInfo)
		return XR_ERROR_INITIALIZATION_FAILED;
	next_get_instance_proc_addr = layer_info->nextInfo->nextGetInstanceProcAddr;
	next_layer_info = *layer_info;
	next_layer_info.nextInfo = layer_info->nextInfo->next;
	return layer_info->nextInfo->nextCreateApiLayerInstance(info, &next_layer_info, instance);
}

__attribute__((visibility("default")))
XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *loader_info,
	const char *layer_name, XrNegotiateApiLayerRequest *request)
{
	(void)layer_name;
	if (!loader_info || !request || loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
		loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
		loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
	{
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
	request->layerApiVersion = loader_info->maxApiVersion < XR_CURRENT_API_VERSION ? loader_info->maxApiVersion :
		XR_CURRENT_API_VERSION;
	request->getInstanceProcAddr = layer_get_instance_proc_addr;
	request->createApiLayerInstance = layer_create_api_layer_instance;
	return XR_SUCCESS;
}
