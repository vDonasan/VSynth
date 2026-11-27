#define MINIAUDIO_IMPLEMENTATION
/* COM em modo "apartment" (STA), e não multithread: os drivers ASIO só
   carregam assim. Com o padrão do miniaudio (MTA) na thread da interface,
   o PortAudio não conseguia abrir nenhum driver e a lista ASIO ficava
   vazia, mesmo com drivers instalados (que funcionam no REAPER).
   0x2 = COINIT_APARTMENTTHREADED. */
#define MA_COINIT_VALUE 0x2
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"
