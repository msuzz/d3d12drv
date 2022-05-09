/**
\class TexConversion
Functions to convert from Unreal format textures to standard R8G8B8A8 Direct3D 11 format inital data.
Uses both Unreal and Direct3D datatypes, but no access to D3D objects like the device etc.

The game has two types of textures: static ones and dynamic. Dynamic texures are parametric ones such as water, etc.
After trying multiple methods, it was determined best to create static textures as USAGE_IMMUTABLE, and dynamic ones as USAGE_DEFAULT (i.e. not USAGE_DYNAMIC).
USAGE_DEFAULT textures cannot be mapped, but they are updated using a copy operation. A nice thing about this is that it allows the texture handling to be streamlined;
it used to have seperate paths for map()-able and immutable textures.
However copy-able and immutable textures can be handled about the same way as they both are created from initial data, instead of being filled after their creation.

Some texture types can be used by D3D without conversion; depending on the type (see formats array below) direct assignments can take place.

New textures are created by having the D3D class create a texture out of their converted/assigned mips stored as D3D_SUBRESOURCE_DATA;
the texture conversion function sets the TEXTURE_2D_DESC parameters for this depending on the texture size, if it is dynamic, etc.
Existing textures are updated by passing a new mip to the D3D class; only the 0th mip is updated, which should be fine (afaik there's no dynamic textures with >1 mips).

Texture conversion functions write to a void pointer so they can work unmodified regardless of the underlying memory.

Additional notes:
- Textures can be updated while a frame is being drawn (i.e. between lock() and unlock()). This means that a texture can even need to be be updated between two successive drawXXXX() calls.
- Textures haven't always the correct 'masked' flag upon initial caching. as such, they must sometimes be replaced if the game later tries to load it with the flag.
	As this cannot be detected in advance, they're created as immutable. Updating is done by deleting and recreating.
- For example dynamic lights have neither bParametric nor bRealtime set. Fortunately, these seem to have bRealtimechanged set initially.
- BRGA7 textures have garbage data outside their UClamp and reading outside the VClamp can lead to access violations. To be able to still direct assign them,
all textures are made only as large as the UClamp*VClamp and the texture coordinates are scaled to reflect this. Furthermore, the D3D_SUBRESOURCE_DATA's stride
parameter is set so the data outside the UClamp is skipped.
*/
#include <stdio.h>
#include <D3DX11.h>
#include "texconversion.h"
#include "polyflags.h"

/**
Mappings from Unreal to our texture info
*/
TexConversion::TextureFormat TexConversion::formats[] = 
{
	{true,0,false,DXGI_FORMAT_R8G8B8A8_UNORM,&TexConversion::fromPaletted},		/**< TEXF_P8 = 0x00 */
	{true,0,true,DXGI_FORMAT_R8G8B8A8_UNORM,NULL},								/**< TEXF_RGBA7	= 0x01 */
	{false,0,true,DXGI_FORMAT_R8G8B8A8_UNORM,NULL},								/**< TEXF_RGB16	= 0x02 */
	{true,4,true,DXGI_FORMAT_BC1_UNORM,NULL},									/**< TEXF_DXT1 = 0x03 */
	{false,0,true,DXGI_FORMAT_UNKNOWN,NULL},									/**< TEXF_RGB8 = 0x04 */
	{true,0,true,DXGI_FORMAT_R8G8B8A8_UNORM,NULL},								/**< TEXF_RGBA8	= 0x05 */
};

/**
Fill texture info structure and execute proper conversion of pixel data.

\param Info Unreal texture information, includes cache id, size information, texture data.
\param PolyFlags Polyflags, see polyflags.h.
*/
void TexConversion::convertAndCache(FTextureInfo& Info,DWORD PolyFlags)
{
	if(Info.Format > TEXF_RGBA8)
	{
		UD3D11RenderDevice::debugs("Unknown texture type.");
		return;
	}
	
	TextureFormat &format=formats[Info.Format];
	if(format.supported == false)
	{
		UD3D11RenderDevice::debugs("Unsupported texture type.");
		return;
	}

	//Set texture info. These parameters are the same for each usage of the texture.
	D3D::TextureMetaData metadata;	
	//Mult is a multiplier (so division is only done once here instead of when texture is applied) to normalize texture coordinates.
	//metadata.width = Info.USize;
	//metadata.height = Info.VSize;	
	//metadata.multU = 1.0 / (Info.UScale * Info.USize);
	//metadata.multV = 1.0 / (Info.VScale * Info.VSize);
	metadata.multU = 1.0 / (Info.UScale * Info.UClamp);
	metadata.multV = 1.0 / (Info.VScale * Info.VClamp);
	metadata.masked = (PolyFlags & PF_Masked)!=0;

	//Convert each mip level
	D3D11_SUBRESOURCE_DATA* data = new D3D11_SUBRESOURCE_DATA[Info.NumMips];
	for(int i=0;i<Info.NumMips;i++)
	{
		convertMip(Info,format,PolyFlags,i,data[i]);
	}

	//Create a texture from the converted data
	bool dynamic = ((Info.bRealtimeChanged || Info.bRealtime || Info.bParametric) != 0);

	D3D11_TEXTURE2D_DESC desc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.ArraySize = 1;
	desc.Height = Info.VClamp;
	desc.Width = Info.UClamp;
	desc.MipLevels = Info.NumMips;
	desc.MiscFlags = 0;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;	
	desc.Format = format.d3dFormat;
	desc.CPUAccessFlags = 0;
	if(dynamic)
		desc.Usage = D3D11_USAGE_DEFAULT;
	else
		desc.Usage = D3D11_USAGE_IMMUTABLE;
	if(format.blocksize>0) //Compressed textures should be a whole amount of blocks
	{
		desc.Width += Info.USize%format.blocksize;
		desc.Height += Info.VSize%format.blocksize;
	}

	ID3D11Texture2D* texture = D3D::createTexture(desc,*data);
	if(texture==NULL)
		return;

	D3D::cacheTexture(Info.CacheID,metadata,texture);

/*
	if(Info.Format == 0  )
	{
		char buf[256];
		sprintf(buf,"d:\\%d.png",Info.CacheID);
		D3DX10SaveTextureToFileA(texture,D3DX10_IFF_PNG,buf);
	}
*/
	//Delete temporary data
	if(!format.directAssign)
	{
		for(int i=0;i<Info.NumMips;i++)
		{
			delete [] data[i].pSysMem;
		}		
	}
	delete [] data;
	SAFE_RELEASE(texture);
}

/**
Update a dynamic texture by converting its 0th mip and letting D3D update it.
*/
void TexConversion::update(FTextureInfo& Info,DWORD PolyFlags)
{	

	D3D11_SUBRESOURCE_DATA data;
	Info.bRealtimeChanged=0; //Clear this flag (from other renderes)
	TextureFormat format = formats[Info.Format];
	convertMip(Info,format,PolyFlags,0,data);
	D3D::updateMip(Info.CacheID,0,data);
	if(!format.directAssign)
		delete [] data.pSysMem;
}

/**
Fills a SUBRESOURCE_DATA structure with converted texture data for a mipmap; if possible, assigns instead of converts.
\param Info Unreal texture info.
\param format Conversion parameters for the texture.
\param PolyFlags Polyflags. See polyflags.h.
\param mipLevel Which mip to convert.
\param data SUBRESOURCE_DATA structure which will be filled.

\note Caller must free data.pSysMem for non-directAssign textures.
*/
void TexConversion::convertMip(FTextureInfo& Info,TextureFormat &format,DWORD PolyFlags,int mipLevel,D3D11_SUBRESOURCE_DATA &data)
{	
	//Set stride
	if(format.blocksize>0)
	{
		data.SysMemPitch=(Info.Mips[mipLevel]->USize)*format.blocksize/2;
	}
	else
	{
		data.SysMemPitch=Info.Mips[mipLevel]->USize*sizeof(DWORD); //Pitch is set so garbage data outside of UClamp is skipped
	}

	//Assign or convert
	if(format.directAssign) //Direct assignment from Unreal to our texture is possible
	{
		data.pSysMem = Info.Mips[mipLevel]->DataPtr;		
	}
	else //Texture needs to be converted via temporary data; allocate it
	{
		data.pSysMem = new DWORD[Info.Mips[mipLevel]->USize*max((Info.VClamp>>mipLevel),1)]; //max(...) as otherwise USize*0 can occur
		if(data.pSysMem==NULL)
		{
			UD3D11RenderDevice::debugs("Convert: Error allocating texture initial data memory.");
			return;
		}				
		//Convert
		format.conversionFunc(Info,PolyFlags,(void*)data.pSysMem,mipLevel);
	}
}

/**
Convert from palleted 8bpp to r8g8b8a8.
*/
void TexConversion::fromPaletted(FTextureInfo& Info,DWORD PolyFlags, void *target,int mipLevel)
{

	//If texture is masked with palette index 0 = transparent; make that index black w. alpha 0 (black looks best for the border that gets left after masking)
	if(PolyFlags & PF_Masked)
	{
		*(DWORD*)(&(Info.Palette->R)) = (DWORD) 0;
	}

	DWORD *dest = (DWORD*) target;
	BYTE *source = (BYTE*) Info.Mips[mipLevel]->DataPtr;
	BYTE *sourceEnd = source + Info.Mips[mipLevel]->USize*Info.Mips[mipLevel]->VSize;
	while(source<sourceEnd)
	{
		*dest=*(DWORD*)&(Info.Palette[*source]);
		source++;
		dest++;
	}

}

/**
BGRA7 to RGBA8. Used for lightmaps and fog. Straightforward, just multiply by 2.
\note IMPORTANT these textures do not have valid data outside of their U/VClamp; there's garbage outside UClamp and reading it outside VClamp sometimes results in access violations.
Unfortunately this means a direct assignment is not possible as we need to manually repeat the rows/columns outside of the clamping range.
\note This format is only used for fog and lightmap; it is also the only format used for those. As such, we can at least do the swizzling and scaling in-shader and use memcpy() here.
\deprecated Direct assignment instead, see text at top of file.
*/
void TexConversion::fromBGRA7(FTextureInfo& Info,DWORD PolyFlags,void *target,int mipLevel)
{/*
	unsigned int VClamp = Info.VClamp>>mipLevel;
	unsigned int UClamp = Info.UClamp>>mipLevel;
	unsigned int USize = Info.Mips[mipLevel]->USize;
	unsigned int VSize = Info.Mips[mipLevel]->VSize;
	DWORD* src =(DWORD*) Info.Mips[mipLevel]->DataPtr;
	DWORD* dst = (DWORD*) target;

	unsigned int row;
	//Copy rows up to VClamp
	for(row=0;row<VClamp;row++)
	{
		memcpy(dst,src,UClamp*sizeof(DWORD)); //Copy valid part
		for(unsigned int col=UClamp;col<USize;col++) //Repeat last pixel as padding
		{
			dst[col]=dst[UClamp-1];
		}
		//Go to next row
		src+=USize;
		dst+=USize;
	}
	
	//Outside row clamp, create copy of last valid row
	for(DWORD* dst2=dst;row<VSize;row++)
	{	
		memcpy(dst2,dst-USize,USize*sizeof(DWORD));
		dst2+=USize;
	}*/
}
