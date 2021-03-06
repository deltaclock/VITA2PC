#include "SDL/SDL.h"
#include "SDL/SDL_image.h"
#include "SDL/SDL_mixer.h"
#include "SDL/SDL_opengl.h"
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __WIN32__
# include <winsock2.h>
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
# include <arpa/inet.h>
#endif

#define VIDEO_PORT     5000     // Port to use for video streaming
#define AUDIO_PORT     4000     // Starting port to use for audio streaming
#define RCV_BUFSIZE    0x800000 // Size of the buffer used to store received packets
#define AUDIO_CHANNELS 8        // PSVITA has 8 available audio channels

// Audioports struct
typedef struct audioPort{
	int len;
	int samplerate;
	int mode;
	uint8_t buffer[RCV_BUFSIZE];
	Mix_Chunk* chunk;
} audioPort;

typedef struct{
	uint32_t sock;
	struct sockaddr_in addrTo;
} Socket;

int width, height, size, samplerate;
SDL_Surface* frame = NULL;
SDL_Surface* new_frame = NULL;
char* buffer;
GLint nofcolors = 3;
GLenum texture_format=GL_RGB;
GLuint texture=0;
char host[32];
static audioPort ports[AUDIO_CHANNELS];
static int thdId[AUDIO_CHANNELS] = {0,1,2,3,4,5,6,7};
static int mix_started = 0;

void updateFrame(){

	// Loading frame
	SDL_RWops* rw = SDL_RWFromMem(buffer,size);
	new_frame = IMG_Load_RW(rw, 1);
	if (new_frame != NULL){
		SDL_FreeSurface(frame);
		frame = new_frame;
	}else printf("\nSDL Error: %s", SDL_GetError());
	if (frame == NULL) return;
	
	fflush(stdout);
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, nofcolors, frame->w, frame->h, 0, texture_format, GL_UNSIGNED_BYTE, frame->pixels );
	
}

// Drawing function using openGL
void drawFrame(){
	if (texture == 0) return;	
	glClear( GL_COLOR_BUFFER_BIT );
	glBindTexture( GL_TEXTURE_2D, texture );
	glBegin( GL_QUADS );
	glTexCoord2i( 0, 0 );
	glVertex3f( 0, 0, 0 );
	glTexCoord2i( 1, 0 );
	glVertex3f( width, 0, 0 );
	glTexCoord2i( 1, 1 );
	glVertex3f( width, height, 0 );
	glTexCoord2i( 0, 1 );
	glVertex3f( 0, height, 0 );
	glEnd();
	glLoadIdentity();
	SDL_GL_SwapBuffers();
}

DWORD WINAPI audioThread(void* data);
Socket* audio_socket[AUDIO_CHANNELS];

void swapChunk_CB(int chn){
	int rbytes;
	do{
		rbytes = recv(audio_socket[chn]->sock, ports[chn].buffer, RCV_BUFSIZE, 0);
	}while(rbytes <= 0);
	
	// Audio port closed on Vita side
	if (rbytes < 512){
		printf("\nAudio channel %d closed", chn);
		CreateThread(NULL, 0, audioThread, &thdId[chn], 0, NULL);
		return;
	}
	
	ports[chn].chunk = Mix_QuickLoad_RAW(ports[chn].buffer, rbytes);
	int err = Mix_PlayChannel(chn, ports[chn].chunk, 0);
	//if (err == -1) printf("\nERROR: Failed outputting audio chunk.\n%s",Mix_GetError());
}

DWORD WINAPI audioThread(void* data) {
	
	int* ptr = (int*)data;
	int id = ptr[0];
	
	printf("\nAudio thread for channel %d started", id);
	
	// Creating client socket
	audio_socket[id] = (Socket*) malloc(sizeof(Socket));
	memset(&audio_socket[id]->addrTo, '0', sizeof(audio_socket[id]->addrTo));
	audio_socket[id]->addrTo.sin_family = AF_INET;
	audio_socket[id]->addrTo.sin_port = htons(AUDIO_PORT + id);
	audio_socket[id]->addrTo.sin_addr.s_addr = inet_addr(host);
	int addrLen = sizeof(audio_socket[id]->addrTo);
	audio_socket[id]->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	
	// Connecting to VITA2PC
	int err = connect(audio_socket[id]->sock, (struct sockaddr*)&audio_socket[id]->addrTo, sizeof(audio_socket[id]->addrTo));
	
	int rbytes;
	do{
		send(audio_socket[id]->sock, "request", 8, 0);
		rbytes = recv(audio_socket[id]->sock, (char*)&ports[id], RCV_BUFSIZE, 0);
	}while (rbytes <= 0);
	
	send(audio_socket[id]->sock, "request", 8, 0);
	audioPort* port = (audioPort*)&ports[id];
	printf("\nAudio thread for port %d operative (Samplerate: %d Hz, Mode: %s)", id, port->samplerate, port->mode == 0 ? "Mono" : "Stereo");
		
	printf("\nStarted Mixer with Samplerate: %d Hz, Mode: %s, Chunk Length: %d", port->samplerate, port->mode == 0 ? "Mono" : "Stereo", port->len);
	err = Mix_OpenAudio(port->samplerate, AUDIO_S16LSB, port->mode + 1, port->len);
	if (err < 0) printf("\nERROR: Failed starting mixer.\n%s",Mix_GetError());
	Mix_ChannelFinished(swapChunk_CB);
	
	int rcvbuf = RCV_BUFSIZE;
	setsockopt(audio_socket[id]->sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	u_long _true = 1;
	ioctlsocket(audio_socket[id]->sock, FIONBIO, &_true);
	do{
		rbytes = recv(audio_socket[id]->sock, port->buffer, RCV_BUFSIZE, 0);
	}while (rbytes <= 0);
	
	port->chunk = Mix_QuickLoad_RAW(port->buffer, rbytes);
	if (port->chunk == NULL) printf("\nERROR: Failed opening audio chunk.\n%s",Mix_GetError());
	int ch = Mix_PlayChannel(id, port->chunk, 0);
	if (ch == -1) printf("\nERROR: Failed outputting audio chunk.\n%s",Mix_GetError());
	else printf("\nStarting audio playback on channel %d. (Chunks size: %d bytes)", ch, rbytes);
	
	return 0;
}

int main(int argc, char* argv[]){

	#ifdef __WIN32__
	WORD versionWanted = MAKEWORD(1, 1);
	WSADATA wsaData;
	WSAStartup(versionWanted, &wsaData);
	#endif
	
	int dummy = 4;
	if (argc > 1){
		char* ip = (char*)(argv[1]);
		sprintf(host,"%s",ip);
	}else{
		printf("Insert Vita IP: ");
		scanf("%s",host);
	}
	
	// Writing info on the screen
	printf("IP: %s\nPort: %d\n\n",host, VIDEO_PORT);
	
	
	// Creating client socket
	Socket* my_socket = (Socket*) malloc(sizeof(Socket));
	memset(&my_socket->addrTo, '0', sizeof(my_socket->addrTo));
	my_socket->addrTo.sin_family = AF_INET;
	my_socket->addrTo.sin_port = htons(VIDEO_PORT);
	my_socket->addrTo.sin_addr.s_addr = inet_addr(host);
	int addrLen = sizeof(my_socket->addrTo);
	my_socket->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (my_socket->sock < 0){
		printf("\nFailed creating socket.");
		return -1;
	}else printf("\nClient socket created on port %d", VIDEO_PORT);
	
	// Connecting to VITA2PC
	int err = connect(my_socket->sock, (struct sockaddr*)&my_socket->addrTo, sizeof(my_socket->addrTo));
	if (err < 0 ){
		printf("\nFailed connecting server.");
		close(my_socket->sock);
		return -1;
	}else printf("\nConnection established!");
	printf("\n\n%d\n\n", err);
	fflush(stdout);
	u_long _true = 1;
	uint8_t accelerated;
	char sizes[32];
	send(my_socket->sock, "request", 8, 0);
	recv(my_socket->sock, sizes, 32, 0);
	sscanf(sizes, "%d;%d;%hhu", &width, &height, &accelerated);
	printf("\nThe game %s hardware acceleration.", accelerated ? "supports" : "does not support");
	printf("\nSetting window resolution to %d x %d", width, height);
	fflush(stdout);
	ioctlsocket(my_socket->sock, FIONBIO, &_true);
	int rcvbuf = RCV_BUFSIZE;
	setsockopt(my_socket->sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	getsockopt(my_socket->sock, SOL_SOCKET, SO_RCVBUF, (char*)(&rcvbuf), &dummy);
	printf("\nReceive buffer size set to %d bytes", rcvbuf);
	
	// Starting audio streaming thread
	int i;
	for (i = 0; i < AUDIO_CHANNELS; i++){
		CreateThread(NULL, 0, audioThread, &thdId[i], 0, NULL);
	}
	
	// Initializing SDL and openGL stuffs
	uint8_t quit = 0;
	SDL_Event event;
	SDL_Surface* screen = NULL;
	SDL_Init( SDL_INIT_EVERYTHING );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	screen = SDL_SetVideoMode( width, height, 32, SDL_OPENGL );
	glClearColor( 0, 0, 0, 0 );
	glEnable( GL_TEXTURE_2D );
	glViewport( 0, 0, width, height );
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0, width, height, 0, -1, 1 );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
	SDL_WM_SetCaption("VITA2PC", NULL);
	
	// Framebuffer & texture stuffs
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	buffer = (uint8_t*)malloc((width*height)<<2);
	
	for (;;){

		// Receiving a new frame
		int rbytes = 0;
		while (rbytes <= 0){
			rbytes = recv(my_socket->sock, buffer, RCV_BUFSIZE, 0);
			while( SDL_PollEvent( &event ) ) {
				if( event.type == SDL_QUIT ) {
					quit = 1;
				} 
			}
			if (quit) break;
			size = rbytes;
		}
		if (quit) break;
		
		updateFrame();
		drawFrame();
		
	}
	
	free(buffer);
	return 0;
}