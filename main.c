#include <libk/string.h>
#include <libk/stdio.h>
#include <libk/ctype.h>
#include <vitasdk.h>
#include <taihen.h>
#include "math_utils.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define HOOKS_NUM 2       // Hooked functions num

// Shaders
#include "shaders/rgba_f.h"
#include "shaders/rgba_v.h"

static uint8_t current_hook;
static SceUID hooks[HOOKS_NUM];
static tai_hook_ref_t refs[HOOKS_NUM];
tai_module_info_t info = {0};

SceGxmShaderPatcher* patcher;

static const SceGxmProgram *const gxm_program_rgba_v = (SceGxmProgram*)&rgba_v;
static const SceGxmProgram *const gxm_program_rgba_f = (SceGxmProgram*)&rgba_f;

static SceGxmShaderPatcherId rgba_vertex_id;
static SceGxmShaderPatcherId rgba_fragment_id;
static const SceGxmProgramParameter* rgba_position;
static const SceGxmProgramParameter* rgba_rgba;
static const SceGxmProgramParameter* wvp;
static SceGxmVertexProgram* rgba_vertex_program_patched;
static SceGxmFragmentProgram* rgba_fragment_program_patched;
static vector3f* rgba_vertices = NULL;
static uint16_t* rgba_indices = NULL;
static SceUID rgba_vertices_uid, rgba_indices_uid;

static vector4f rect_rgba;
static matrix4x4 mvp;

void* gpu_alloc_map(SceKernelMemBlockType type, SceGxmMemoryAttribFlags gpu_attrib, size_t size, SceUID *uid){
	SceUID memuid;
	void *addr;

	if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)
		size = ALIGN(size, 256 * 1024);
	else
		size = ALIGN(size, 4 * 1024);

	memuid = sceKernelAllocMemBlock("gpumem", type, size, NULL);
	if (memuid < 0)
		return NULL;

	if (sceKernelGetMemBlockBase(memuid, &addr) < 0)
		return NULL;

	if (sceGxmMapMemory(addr, size, gpu_attrib) < 0) {
		sceKernelFreeMemBlock(memuid);
		return NULL;
	}

	if (uid)
		*uid = memuid;

	return addr;
}

// Simplified generic hooking function
void hookFunction(uint32_t nid, const void *func){
	hooks[current_hook] = taiHookFunctionImport(&refs[current_hook],TAI_MAIN_MODULE,TAI_ANY_LIBRARY,nid,func);
	current_hook++;
}

int sceGxmShaderPatcherCreate_patched(const SceGxmShaderPatcherParams *params, SceGxmShaderPatcher **shaderPatcher){
	int res =  TAI_CONTINUE(int, refs[0], params, shaderPatcher);
	
	// Grabbing a reference to used shader patcher
	patcher = *shaderPatcher;
	
	// Registering our shaders
	sceGxmShaderPatcherRegisterProgram(
		patcher,
		gxm_program_rgba_v,
		&rgba_vertex_id);
	sceGxmShaderPatcherRegisterProgram(
		patcher,
		gxm_program_rgba_f,
		&rgba_fragment_id);
		
	// Getting references to our vertex streams/uniforms
	rgba_position = sceGxmProgramFindParameterByName(gxm_program_rgba_v, "aPosition");
	rgba_rgba = sceGxmProgramFindParameterByName(gxm_program_rgba_f, "color");
	wvp = sceGxmProgramFindParameterByName(gxm_program_rgba_v, "wvp");
	
	// Setting up our vertex stream attributes
	SceGxmVertexAttribute rgba_vertex_attribute;
	SceGxmVertexStream rgba_vertex_stream;
	rgba_vertex_attribute.streamIndex = 0;
	rgba_vertex_attribute.offset = 0;
	rgba_vertex_attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	rgba_vertex_attribute.componentCount = 3;
	rgba_vertex_attribute.regIndex = sceGxmProgramParameterGetResourceIndex(rgba_position);
	rgba_vertex_stream.stride = sizeof(vector3f);
	rgba_vertex_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

	// Creating our shader programs
	sceGxmShaderPatcherCreateVertexProgram(patcher,
		rgba_vertex_id, &rgba_vertex_attribute,
		1, &rgba_vertex_stream, 1, &rgba_vertex_program_patched);
	sceGxmShaderPatcherCreateFragmentProgram(patcher,
		rgba_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, NULL, gxm_program_rgba_f,
		&rgba_fragment_program_patched);
	
	// Allocating default vertices/indices on CDRAM
	rgba_vertices = gpu_alloc_map(
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SCE_GXM_MEMORY_ATTRIB_READ,
		4 * sizeof(vector3f), &rgba_vertices_uid);
	rgba_indices = gpu_alloc_map(
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SCE_GXM_MEMORY_ATTRIB_READ,
		4 * sizeof(unsigned short), &rgba_indices_uid);
		
	// Setting up default vertices
	rgba_vertices[0].x = 0.0f;
	rgba_vertices[0].y = 0.0f;
	rgba_vertices[0].z = 0.5f;
	rgba_vertices[1].x = 0.0f;
	rgba_vertices[1].y = 544.0f;
	rgba_vertices[1].z = 0.5f;
	rgba_vertices[2].x = 960.0f;
	rgba_vertices[2].y = 544.0f;
	rgba_vertices[2].z = 0.5f;
	rgba_vertices[3].x = 960.0f;
	rgba_vertices[3].y = 0.0f;
	rgba_vertices[3].z = 0.5f;
	
	// Setting up default indices
	int i;
	for (i=0;i<4;i++){
		rgba_indices[i] = i;
	}
	
	// Setting up default modelviewprojection matrix
	matrix4x4 projection, modelview;
	matrix4x4_identity(modelview);
	matrix4x4_init_orthographic(projection, 0, 960, 544, 0, -1, 1);
	matrix4x4_multiply(mvp, projection, modelview);
	
	return res;
}

int sceGxmEndScene_patched(SceGxmContext *context, const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification){
	
	// Updating our rectangle alpha value
	SceDateTime time;
	sceRtcGetCurrentClockLocalTime(&time);
	if (time.hour < 6)		// Night/Early Morning
		rect_rgba.a = 0.25f;
	else if (time.hour < 10) // Morning/Early Day
		rect_rgba.a = 0.1f;
	else if (time.hour < 15) // Mid day
		rect_rgba.a = 0.05f;
	else if (time.hour < 19) // Late day
		rect_rgba.a = 0.15f;
	else // Evening/Night
		rect_rgba.a = 0.2f;
	
	/* Before ending scene, we draw our stuffs */
	
	// Setting up desired shaders
	sceGxmSetVertexProgram(context, rgba_vertex_program_patched);
	sceGxmSetFragmentProgram(context, rgba_fragment_program_patched);
	
	// Setting vertex stream and uniform values
	void *rgba_buffer, *wvp_buffer;
	sceGxmReserveFragmentDefaultUniformBuffer(context, &rgba_buffer);
	sceGxmSetUniformDataF(rgba_buffer, rgba_rgba, 0, 4, &rect_rgba.r);
	sceGxmReserveVertexDefaultUniformBuffer(context, &wvp_buffer);
	sceGxmSetUniformDataF(wvp_buffer, wvp, 0, 16, (const float*)mvp);
	sceGxmSetVertexStream(context, 0, rgba_vertices);
	
	// Scheduling a draw command
	sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, rgba_indices, 4);
	
	return TAI_CONTINUE(int, refs[1], context, vertexNotification, fragmentNotification);
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	
	// Setting up default color
	rect_rgba.r = 1.0f;
	rect_rgba.g = 0.5f;
	rect_rgba.b = 0.0f;
	
	// Hooking functions
	hookFunction(0x05032658, sceGxmShaderPatcherCreate_patched);
	hookFunction(0xFE300E2F, sceGxmEndScene_patched);
	
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	
	// Freeing hooks
	while (current_hook-- > 0){
		taiHookRelease(hooks[current_hook], refs[current_hook]);
	}
	
	return SCE_KERNEL_STOP_SUCCESS;	
}