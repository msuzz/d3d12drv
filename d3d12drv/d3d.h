/**
\file d3d.h
*/

#pragma once
#define SAFE_RELEASE(p)      { if(p) { (p)->Release(); (p)=NULL; } }
#define CLAMP(p,min,max)	{ if(p < min) p = min; else if (p>max) p = max; }

#include <D3D12.h>
#include <D3DX11Effect.h> // TODO: Find D3D12 replacement

class D3D
{
private:
	static int createRenderTargetViews();
	static int findAALevel();
	static void commit();
	static ID3DX11EffectPass* switchToPass(int index); // TODO: Find D3D12 replacement
	
public:
	/**
	List of texture passes that can be bound.
	\note DUMMY_NUM_PASSES is so arrays can be indexed etc. for each pass.
	*/
	enum TexturePass {PASS_DIFFUSE,PASS_LIGHT,PASS_DETAIL,PASS_FOG,PASS_MACRO,DUMMY_NUM_PASSES}; 

	/**
	Projection modes. 
	PROJ_NORMAL is normal projection.
	PROJ_Z_ONLY only applies the projection to the Z coordinate.
	PROJ_COMPENSATE_Z_NEAR projects vertices but adjusts their W value to compensate for being moved inside zNear
	*/
	enum ProjectionMode {PROJ_NORMAL,PROJ_Z_ONLY,PROJ_COMPENSATE_Z_NEAR};

	/**
	Custom flags to set render state from renderer interface.
	*/
	//enum Polyflag {};

	/** 2 float vector */
	struct Vec2
	{
		float x,y;
	};

	/** 3 float vector */
	struct Vec3
	{
		float x,y,z;
	};

	/** 4 float vector */
	struct Vec4
	{
		float x,y,z,w;
	};

	/** 4 byte vector */
	struct Vec4_byte
	{
		BYTE x,y,z,w;
	};

	/** Vertex */
	struct Vertex
	{
		Vec3 Pos;
		Vec4 Color;
		Vec4 Fog;
		Vec3 Normal;
		Vec2 TexCoord[D3D::DUMMY_NUM_PASSES];
		DWORD flags;
	};

	/** Most basic vertex for post processing */
	struct SimpleVertex
	{
		Vec3 Pos;		
	};

	/** Texture metadata stored and retrieved with cached textures */
	struct TextureMetaData
	{
		//UINT height;
		//UINT width;

		/** Precalculated parameters with which to normalize texture coordinates */
		FLOAT multU;
		FLOAT multV;
		bool masked; /**< Tracked to fix masking issues, see UD3D11RenderDevice::PrecacheTexture */
	};

	/** Cached, API format texture */
	struct CachedTexture
	{
		TextureMetaData metadata;
		ID3D11ShaderResourceView* resourceView;
	};

	/** Options, some user configurable */
	static struct Options
	{
		int samples; /**< Number of MSAA samples */
		int VSync; /**< VSync on/off */
		int refresh; /**< Refresh rate **/
		int aniso; /**< Anisotropic filtering levels */
		int LODBias; /**< Mipmap LOD bias */
		float brightness; /**< Game brightness */
		int POM; /**< Parallax occlusion mapping */
		int alphaToCoverage; /**< Alpha to coverage support */
		float zNear; /**< Near Z value used in shader and for projection matrix */
	};
	
	/**@name API initialization/upkeep */
	//@{
	static int init(HWND hwnd,D3D::Options &createOptions);
	static void uninit();
	static int resize(int X, int Y, bool fullScreen);
	//@}
	
	/**@name Setup/clear frame */
	//@{
	static void newFrame();
	static void clear(D3D::Vec4& clearColor);
	static void clearDepth();
	//@}

	/**@name Prepare and render buffers */
	//@{
	static void map(bool clear);
	static void render();
	static void present();
	//@}

	/**@name Index and buffer vertices */
	//@{
	static void indexTriangleFan(int num);
	static void indexQuad();
	static D3D::Vertex* getVertex();
	
	//@}
	
	/**@name Set state (projection, flags) */
	//@{
	static void setViewPort(int X, int Y, int left, int top);
	static void setProjectionMode(D3D::ProjectionMode mode);
	static void setProjection(float aspect, float XoverZ);
	static void setFlags(int flags, int d3dflags);
	//@}
	
	/**@name Texture cache */
	//@{
	static ID3D11Texture2D *createTexture(D3D11_TEXTURE2D_DESC &desc, D3D11_SUBRESOURCE_DATA &data);
	static void updateMip(DWORD64 id,int mipNum,D3D11_SUBRESOURCE_DATA &data);
	static void cacheTexture(DWORD64 id,TextureMetaData &metadata,ID3D11Texture2D *tex);
	static bool textureIsCached(DWORD64 id);	
	static D3D::TextureMetaData &getTextureMetaData(DWORD64 id);
	static D3D::TextureMetaData *setTexture(D3D::TexturePass pass,DWORD64 id);
	static void deleteTexture(DWORD64 id);
	static void flush();
	//@}

	/**@name Misc */
	//@{
	static void flash(bool enable,D3D::Vec4 &color);
	static void fog(float dist, D3D::Vec4 *color);
	static TCHAR *getModes();
	static void getScreenshot(D3D::Vec4_byte* buf);
	static void setBrightness(float brightness);
	//@}
};