/*
	PowerVR interface to plugins
	Handles YUV conversion (slow and ugly -- but hey it works ...)

	Most of this was hacked together when i needed support for YUV-dma for thps2 ;)
*/
#include "pvr_mem.h"
#include "Renderer_if.h"
#include "ta.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"

static u32 pvr_map32(u32 offset32);

//YUV converter code :)
//inits the YUV converter
u32 YUV_tempdata[512/4];//512 bytes

u32 YUV_dest=0;

u32 YUV_blockcount;

u32 YUV_x_curr;
u32 YUV_y_curr;

u32 YUV_x_size;
u32 YUV_y_size;

static u32 YUV_index = 0;

void YUV_init()
{
	YUV_x_curr=0;
	YUV_y_curr=0;

	YUV_dest = TA_YUV_TEX_BASE & VRAM_MASK;
	TA_YUV_TEX_CNT=0;
	YUV_blockcount = (TA_YUV_TEX_CTRL.yuv_u_size + 1) * (TA_YUV_TEX_CTRL.yuv_v_size + 1);

	if (TA_YUV_TEX_CTRL.yuv_tex != 0)
	{
		die ("YUV: Not supported configuration\n");
		YUV_x_size=16;
		YUV_y_size=16;
	}
	else
	{
		YUV_x_size = (TA_YUV_TEX_CTRL.yuv_u_size + 1) * 16;
		YUV_y_size = (TA_YUV_TEX_CTRL.yuv_v_size + 1) * 16;
	}
	YUV_index = 0;
}

static void YUV_Block8x8(u8* inuv, u8* iny, u8* out)
{
	u8* line_out_0=out+0;
	u8* line_out_1=out+YUV_x_size*2;

	for (int y=0;y<8;y+=2)
	{
		for (int x=0;x<8;x+=2)
		{
			u8 u=inuv[0];
			u8 v=inuv[64];

			line_out_0[0]=u;
			line_out_0[1]=iny[0];
			line_out_0[2]=v;
			line_out_0[3]=iny[1];

			line_out_1[0]=u;
			line_out_1[1]=iny[8+0];
			line_out_1[2]=v;
			line_out_1[3]=iny[8+1];

			inuv+=1;
			iny+=2;

			line_out_0+=4;
			line_out_1+=4;
		}
		iny+=8;
		inuv+=4;

		line_out_0+=YUV_x_size*4-8*2;
		line_out_1+=YUV_x_size*4-8*2;
	}
}

static INLINE void YUV_Block384(u8* in, u8* out)
{
	u8* inuv=in;
	u8* iny=in+128;
	u8* p_out=out;

	YUV_Block8x8(inuv+ 0,iny+  0,p_out);                    //(0,0)
	YUV_Block8x8(inuv+ 4,iny+64,p_out+8*2);                 //(8,0)
	YUV_Block8x8(inuv+32,iny+128,p_out+YUV_x_size*8*2);     //(0,8)
	YUV_Block8x8(inuv+36,iny+192,p_out+YUV_x_size*8*2+8*2); //(8,8)
}

static INLINE void YUV_ConvertMacroBlock(u8* datap)
{
	//do shit
	TA_YUV_TEX_CNT++;

	YUV_Block384((u8*)datap,vram.data + YUV_dest);

	YUV_dest+=32;

	YUV_x_curr+=16;
	if (YUV_x_curr==YUV_x_size)
	{
		YUV_dest+=15*YUV_x_size*2;
		YUV_x_curr=0;
		YUV_y_curr+=16;
		if (YUV_y_curr==YUV_y_size)
		{
			YUV_y_curr=0;
		}
	}

	if (YUV_blockcount==TA_YUV_TEX_CNT)
	{
		YUV_init();
		
		asic_RaiseInterrupt(holly_YUV_DMA);
	}
}

void YUV_data(u32* data , u32 count)
{
	if (YUV_blockcount==0)
	{
		die("YUV_data : YUV decoder not inited , *WATCH*\n");
		//wtf ? not inited
		YUV_init();
	}

	u32 block_size = TA_YUV_TEX_CTRL.yuv_form == 0 ? 384 : 512;

	verify(block_size==384); //no support for 512

	count*=32;

	while (count > 0)
	{
		if (YUV_index + count >= block_size)
		{
			//more or exactly one block remaining
			u32 dr = block_size - YUV_index;				//remaining bytes til block end
			if (YUV_index == 0)
			{
				// Avoid copy
				YUV_ConvertMacroBlock((u8 *)data);				//convert block
			}
			else
			{
				memcpy(&YUV_tempdata[YUV_index >> 2], data, dr);//copy em
				YUV_ConvertMacroBlock((u8 *)&YUV_tempdata[0]);	//convert block
				YUV_index = 0;
			}
			data += dr >> 2;									//count em
			count -= dr;
		}
		else
		{	//less that a whole block remaining
			memcpy(&YUV_tempdata[YUV_index >> 2], data, count);	//append it
			YUV_index += count;
			count = 0;
		}
	}

	verify(count==0);
}

//vram 32-64b

//read
template<typename T>
T DYNACALL pvr_read_area1(u32 addr)
{
	return *(T *)&vram[pvr_map32(addr)];
}
template u8 pvr_read_area1<u8>(u32 addr);
template u16 pvr_read_area1<u16>(u32 addr);
template u32 pvr_read_area1<u32>(u32 addr);

//write
template<typename T>
void DYNACALL pvr_write_area1(u32 addr, T data)
{
	if (sizeof(T) == 1)
	{
		INFO_LOG(MEMORY, "%08x: 8-bit VRAM writes are not possible", addr);
		return;
	}
	u32 vaddr = addr & VRAM_MASK;
	if (vaddr >= fb_watch_addr_start && vaddr < fb_watch_addr_end)
		fb_dirty = true;

	*(T *)&vram[pvr_map32(addr)] = data;
}
template void pvr_write_area1<u8>(u32 addr, u8 data);
template void pvr_write_area1<u16>(u32 addr, u16 data);
template void pvr_write_area1<u32>(u32 addr, u32 data);

void TAWrite(u32 address, u32* data, u32 count)
{
	if ((address & 0x800000) == 0)
		// TA poly
		ta_vtx_data(data, count);
	else
		// YUV Converter
		YUV_data(data, count);
}

#if HOST_CPU!=CPU_ARM
extern "C" void DYNACALL TAWriteSQ(u32 address, u8* sqb)
{
	u32 address_w = address & 0x01FFFFE0;
	u8* sq = &sqb[address & 0x20];

	if (likely(address_w < 0x800000))//TA poly
	{
		ta_vtx_data32(sq);
	}
	else if(likely(address_w < 0x1000000)) //Yuv Converter
	{
		YUV_data((u32*)sq, 1);
	}
	else //Vram Writef
	{
		// Used by WinCE
		DEBUG_LOG(MEMORY, "Vram TAWriteSQ 0x%X SB_LMMODE0 %d", address, SB_LMMODE0);
		bool path64b = (address & 0x02000000 ? SB_LMMODE1 : SB_LMMODE0) == 0;
		if (path64b)
		{
			// 64b path
			memcpy(&vram[address_w & VRAM_MASK], sq, 32);
		}
		else
		{
			// 32b path
			for (int i = 0; i < 8; i++, address_w += 4)
				pvr_write_area1<u32>(address_w, ((u32 *)sq)[i]);
		}
	}
}
#endif

//Misc interface

#define VRAM_BANK_BIT 0x400000

static u32 pvr_map32(u32 offset32)
{
	//64b wide bus is achieved by interleaving the banks every 32 bits
	const u32 static_bits = VRAM_MASK - (VRAM_BANK_BIT * 2 - 1) + 3;
	const u32 offset_bits = (VRAM_BANK_BIT - 1) & ~3;

	u32 bank = (offset32 & VRAM_BANK_BIT) / VRAM_BANK_BIT;

	u32 rv = offset32 & static_bits;

	rv |= (offset32 & offset_bits) * 2;

	rv |= bank * 4;
	
	return rv;
}


f32 vrf(u32 addr)
{
	return *(f32*)&vram[pvr_map32(addr)];
}
u32 vri(u32 addr)
{
	return *(u32*)&vram[pvr_map32(addr)];
}

template<typename T, bool upper>
T DYNACALL pvr_read_area4(u32 addr)
{
	bool access32 = (upper ? SB_LMMODE1 : SB_LMMODE0) == 1;
	if (access32)
		return pvr_read_area1<T>(addr);
	else
		return *(T*)&vram[addr & VRAM_MASK];
}
template u8 pvr_read_area4<u8, false>(u32 addr);
template u16 pvr_read_area4<u16, false>(u32 addr);
template u32 pvr_read_area4<u32, false>(u32 addr);
template u8 pvr_read_area4<u8, true>(u32 addr);
template u16 pvr_read_area4<u16, true>(u32 addr);
template u32 pvr_read_area4<u32, true>(u32 addr);

template<typename T, bool upper>
void DYNACALL pvr_write_area4(u32 addr, T data)
{
	bool access32 = (upper ? SB_LMMODE1 : SB_LMMODE0) == 1;
	if (access32)
		pvr_write_area1(addr, data);
	else
		*(T*)&vram[addr & VRAM_MASK] = data;
}
template void pvr_write_area4<u8, false>(u32 addr, u8 data);
template void pvr_write_area4<u16, false>(u32 addr, u16 data);
template void pvr_write_area4<u32, false>(u32 addr, u32 data);
template void pvr_write_area4<u8, true>(u32 addr, u8 data);
template void pvr_write_area4<u16, true>(u32 addr, u16 data);
template void pvr_write_area4<u32, true>(u32 addr, u32 data);
