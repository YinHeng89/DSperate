// The attachments the tail and mask shaders read back: one sample per
// invocation under 4x MSAA (gl_SampleID makes the stage run per sample).
#ifdef DS_MSAA
#define DS_INPUT usubpassInputMS
#define DS_LOAD(x) subpassLoad(x, gl_SampleID)
#else
#define DS_INPUT usubpassInput
#define DS_LOAD(x) subpassLoad(x)
#endif
