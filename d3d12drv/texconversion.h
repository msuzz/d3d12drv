/**
\file texconversion.h
*/

#pragma once
#include "d3d.h"
#include "D3D12Drv.h"

class TexConversion
{
private:

	/**
	Format for a texture, tells the conversion functions if data should be allocated, block sizes taken into account, etc
	*/
	struct TextureFormat
	{
		bool supported; /**< Is format supported by us */
		char blocksize; /**< Block size for compressed textures */
		bool directAssign; /**< No conversion and temporary storage needed */
		DXGI_FORMAT d3dFormat; /**< D3D format to use when creating texture */
		void (*conversionFunc)(FTextureInfo&, DWORD, void *, int);	/**< Conversion function to use if no direct assignment possible */
	};
	static TexConversion::TextureFormat formats[];

	/**@name Format conversion functions */
	//@{
	static void fromPaletted(FTextureInfo& Info,DWORD PolyFlags,void *target, int mipLevel);
	static void fromBGRA7(FTextureInfo& Info,DWORD PolyFlags,void *target,int mipLevel);
	//@}

	static void convertMip(FTextureInfo& Info,TextureFormat &format, DWORD PolyFlags,int mipLevel, D3D11_SUBRESOURCE_DATA &data);
	
public:
	static void convertAndCache(FTextureInfo& Info, DWORD PolyFlags);
	static void update(FTextureInfo& Info,DWORD PolyFlags);

};