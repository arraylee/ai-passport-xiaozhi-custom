#include "webrtc/common_audio/vad/vad_core.h"
#include <stdio.h>
int main(void){VadInstT vad={0};short samples[80]={0};WebRtcVad_InitCore(&vad);printf("vad state=%zu silence=%d\n",sizeof vad,WebRtcVad_CalcVad8khz(&vad,samples,80));return 0;}
