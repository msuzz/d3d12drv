/**
\class UD3D12RenderDevice

Old documentation for UD3D11RenderDevice:

This is the interface between the game and the graphics API.
For the D3D11 renderer, an effort was made to have it not work directly with D3D types and objects; it is purely concerned with answering the game and putting
data in correct structures for further processing. This leaves this class relatively clean and easy to understand, and should make it a good basis for further work.
It contains only the bare essential functions to implement the renderer interface.
There are two exceptions: UD3D11RenderDevice::debugs() and UD3D11RenderDevice::getOption are helpers not required by the game.

Called UD3D11RenderDevice as Unreal leaves out first letter when accessing the class; now it can be accessed as D3D11RenderDevice.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <FCNTL.H>
#include "resource.h"
#include "D3D12Drv.h"
#include "texconversion.h"
#include "customflags.h"
#include "misc.h"


//UObject glue
IMPLEMENT_PACKAGE(D3D12Drv);
IMPLEMENT_CLASS(UD3D12RenderDevice);

static bool drawingWeapon; /** Whether the depth buffer was cleared and projection parameters set to draw the weapon model */
static int customFOV; /**Field of view calculated from aspect ratio */
/** See SetSceneNode() */
const float Z_NEAR = 7.0f;

/**
Prints text to the game's log and the standard output if in debug mode.
\param s A the message to print.
\note Does not take a wide character string because not everything we want to print might be available as such (i.e. ID3D10Blobs).
*/
void UD3D12RenderDevice::debugs(char* s)
{ 
	WCHAR buf[255];
	size_t n;
	mbstowcs_s(&n,buf,255,s,254);
	GLog->Log(buf);
	#ifdef _DEBUG //In debug mode, print output to console
	puts(s);
	#endif
}

/**
Attempts to read a property from the game's config file; on failure, a default is written (so it can be changed by the user) and returned.
\param name A string identifying the config file options.
\param defaultVal The default value to write and return if the option is not found.
\param isBool Whether the parameter's a boolean or integer
\return The value for the property.
\note The default value is written so it can be user modified (either from the config or preferences window) from then on.
*/
int UD3D12RenderDevice::getOption(TCHAR* name,int defaultVal, bool isBool)
{
	TCHAR* Section = L"D3D12Drv.D3D12RenderDevice";
	int out;
	if(isBool)
	{
		if(!GConfig->GetBool( Section, name, (INT&) out))
		{
			GConfig->SetBool(Section,name,defaultVal);
			out = defaultVal;
		}
	}
	else
	{
		if(!GConfig->GetInt( Section, name, (INT&) out))
		{
			GConfig->SetInt(Section,name,defaultVal);
			out = defaultVal;
		}
	}
	return out;
}

/**
Constructor called by the game when the renderer is first created.
\note Required to compile for Unreal Tournament. 
\note Binding settings to the preferences window needs to done here instead of in init() or the game crashes when starting a map if the renderer's been restarted at least once.
*/
void UD3D12RenderDevice::StaticConstructor()
{
	//Make the property appear in the preferences window; this will automatically pick up the current value and write back changes.
	new(GetClass(), L"Precache", RF_Public) UBoolProperty(CPP_PROPERTY(options.precache), TEXT("Options"), CPF_Config);

	new(GetClass(), L"Antialiasing", RF_Public) UIntProperty(CPP_PROPERTY(D3DOptions.samples), TEXT("Options"), CPF_Config);
	new(GetClass(), L"Anisotropy", RF_Public) UIntProperty(CPP_PROPERTY(D3DOptions.aniso), TEXT("Options"), CPF_Config);
	new(GetClass(), L"VSync", RF_Public) UBoolProperty(CPP_PROPERTY(D3DOptions.VSync), TEXT("Options"), CPF_Config);
	new(GetClass(), L"RefreshRate", RF_Public) UIntProperty(CPP_PROPERTY(D3DOptions.refresh), TEXT("Options"), CPF_Config);
	new(GetClass(), L"ParallaxOcclusionMapping", RF_Public) UBoolProperty(CPP_PROPERTY(D3DOptions.POM), TEXT("Options"), CPF_Config);
	new(GetClass(), L"LODBias", RF_Public) UIntProperty(CPP_PROPERTY(D3DOptions.LODBias), TEXT("Options"), CPF_Config);
	new(GetClass(), L"AlphaToCoverage", RF_Public) UBoolProperty(CPP_PROPERTY(D3DOptions.alphaToCoverage), TEXT("Options"), CPF_Config);

	//Create a console to print debug stuff to.
	#ifdef _DEBUG
	AllocConsole();
	stdout->_file = _open_osfhandle((long)GetStdHandle(STD_OUTPUT_HANDLE),_O_TEXT);
	#endif
}

/**
Initialization of renderer.
- Set parent class options. Some of these are settings for the renderer to heed, others control what the game does.
	- URenderDevice::SpanBased; Probably for software renderers.
	- URenderDevice::Fullscreen; Only for Voodoo cards.
	- URenderDevice::SupportsTC; Game sends compressed textures if present.
	- URenderDevice::SupportsDistanceFog; Distance fog. Don't know how this is supposed to be implemented.
	- URenderDevice::SupportsLazyTextures; Renderer loads and unloads texture info when needed (???).
	- URenderDevice::PrefersDeferredLoad; Renderer prefers not to cache textures in advance (???).
	- URenderDevice::ShinySurfaces; Renderer supports detail textures. The game sends them always, so it's meant as a detail setting for the renderer.
	- URenderDevice::Coronas; If enabled, the game draws light coronas.
	- URenderDevice::HighDetailActors; If enabled, game sends more detailed models (???).
	- URenderDevice::VolumetricLighting; If enabled, the game sets fog textures for surfaces if needed.
	- URenderDevice::PrecacheOnFlip; The game will call the PrecacheTexture() function to load textures in advance. Also see Flush().
	- URenderDevice::Viewport; Always set to InViewport.
- Initialize graphics api.
- Resize buffers (convenient to use SetRes() for this).

\param InViewport viewport parameters, can get the window handle.
\param NewX Viewport width.
\param NewY Viewport height.
\param NewColorBytes Color depth.
\param Fullscreen Whether fullscreen mode should be used.
\return 1 if init succesful. On 0, game errors out.

\note D3D10 renderer ignores color depth.
*/
UBOOL UD3D12RenderDevice::Init(UViewport *InViewport,INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{
	UD3D12RenderDevice::debugs("Initializing Direct3D 11 renderer.");
	
	//Set parent class params
	URenderDevice::SpanBased = 0;
	URenderDevice::FullscreenOnly = 0;
	URenderDevice::SupportsFogMaps = 1;
	URenderDevice::SupportsTC = 1;
	URenderDevice::SupportsDistanceFog = 0;
	URenderDevice::SupportsLazyTextures = 0;

	//Force on detail options as not all games give easy access to these
	URenderDevice::Coronas = 1;
	#if (!UNREALGOLD)
	URenderDevice::DetailTextures = 1;
	#endif
	URenderDevice::ShinySurfaces = 1;
	URenderDevice::HighDetailActors = 1;
	URenderDevice::VolumetricLighting = 1;
	//Make options reflect this
	GConfig->SetBool(L"D3D12Drv.D3D12RenderDevice",L"Coronas",1);
	GConfig->SetBool(L"D3D12Drv.D3D12RenderDevice",L"DetailTextures",1);
	GConfig->SetBool(L"D3D12Drv.D3D12RenderDevice",L"ShinySurfaces",1);
	GConfig->SetBool(L"D3D12Drv.D3D12RenderDevice",L"HighDetailActors",1);
	GConfig->SetBool(L"D3D12Drv.D3D12RenderDevice",L"VolumetricLighting",1);
	

	//Get/set config options.
	options.precache = getOption(L"Precache",0,true);
	D3DOptions.samples = getOption(L"Antialiasing",4,false);
	D3DOptions.aniso = getOption(L"Anisotropy",8,false);
	D3DOptions.VSync = getOption(L"VSync",1,true);	
	D3DOptions.refresh = getOption(L"RefreshRate", 1, true);
	D3DOptions.LODBias = getOption(L"LODBias",0,false);
	int atocDefault=0;
	#if(DEUSEX) //Enable alpha to coverage for deus ex as it doens't have obvious glitching skyboxes
	atocDefault=1;
	#endif
	D3DOptions.alphaToCoverage = getOption(L"AlphaToCoverage",atocDefault,true);
	GConfig->GetFloat(L"WinDrv.WindowsClient",L"Brightness",D3DOptions.brightness);
	D3DOptions.zNear = Z_NEAR;
	 
	//Set parent options
	URenderDevice::Viewport = InViewport;

	//Do some nice compatibility fixing: set processor affinity to single-cpu
	SetProcessAffinityMask(GetCurrentProcess(),0x1);

	//Initialize Direct3D
	if(!D3D::init((HWND) InViewport->GetWindow(),D3DOptions))
	{
		GError->Log(L"Init: Initializing Direct3D failed.");
		return 0;
	}

	

	if(!UD3D12RenderDevice::SetRes(NewX,NewY,NewColorBytes,Fullscreen))
	{
		GError->Log(L"Init: SetRes failed.");
		return 0;
	}
	

	URenderDevice::PrecacheOnFlip = 1; //Turned on to immediately recache on init (prevents lack of textures after fullscreen switch)

	D3D::setFlags(0,0);
	return 1;
}

/**
Resize buffers and viewport.
\return 1 if resize succesful. On 0, game errors out.

\note Switching to fullscreen exits and reinitializes the renderer.
\note Fullscreen can have values other than 0 and 1 for some reason.
\note This function MUST call URenderDevice::Viewport->ResizeViewport() or the game will stall.
*/
UBOOL UD3D12RenderDevice::SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen)
{

	//Without BLIT_Direct3D major flickering occurs when switching from fullscreen to windowed.
	UBOOL Result = URenderDevice::Viewport->ResizeViewport(Fullscreen ? (BLIT_Fullscreen|BLIT_Direct3D) : (BLIT_HardwarePaint|BLIT_Direct3D), NewX, NewY, NewColorBytes);
	if (!Result) 
	{
		GError->Log(L"SetRes: Error resizing viewport.");
		return 0;
	}	
	if(!D3D::resize(NewX,NewY,(Fullscreen!=0)))
	{
		GError->Log(L"SetRes: D3D::Resize failed.");
		return 0;
	}
	
	//Calculate new FOV. Is set, if needed, at frame start as game resets FOV on level load.
	int defaultFOV;
	#if(RUNE||DEUSEX)
		defaultFOV=75;
	#endif
	#if(UNREALGOLD||UNREALTOURNAMENT)
		defaultFOV=90;
	#endif
	customFOV = Misc::getFov(defaultFOV,Viewport->SizeX,Viewport->SizeY);

	return 1;
}

/**
Cleanup.
*/
void UD3D12RenderDevice::Exit()
{
	UD3D12RenderDevice::debugs("Direct3D 12 renderer exiting.");
	D3D::uninit();
	//FreeConsole();
}

/**
Empty texture cache.
\param AllowPrecache Enabled if the game allows us to precache; respond by setting URenderDevice::PrecacheOnFlip = 1 if wanted. This does make load times longer.
*/
#if UNREALGOLD
void UD3D12RenderDevice::Flush()
#else
void UD3D12RenderDevice::Flush(UBOOL AllowPrecache)
#endif
{
	D3D::flush();

	//If caching is allowed, tell the game to make caching calls (PrecacheTexture() function)
	#if (!UNREALGOLD)
	if(AllowPrecache && options.precache)
		URenderDevice::PrecacheOnFlip = 1;
	#endif
}

/**
Clear screen and depth buffer, prepare buffers to receive data.
\param FlashScale To do with flash effects, see notes.
\param FlashFog To do with flash effects, see notes.
\param ScreenClear The color with which to clear the screen. Used for Rune fog.
\param RenderLockFlags Signify whether the screen should be cleared. Depth buffer should always be cleared.
\param InHitData Something to do with clipping planes; safe to ignore.
\param InHitSize Something to do with clipping planes; safe to ignore.

\note 'Flash' effects are fullscreen colorization, for example when the player is underwater (blue) or being hit (red).
Depending on the values of the related parameters (see source code) this should be drawn; the games don't always send a blank flash when none should be drawn.
EndFlash() ends this, but other renderers actually save the parameters and start drawing it there (probably so it is drawn with the correct depth).
\note RenderLockFlags aren't always properly set, this results in for example glitching in the Unreal castle flyover, in the wall of the tower with the Nali on it.
*/
void UD3D12RenderDevice::Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{

	//If needed, set new field of view; the game resets this on level switches etc. Can't be done in config as Unreal doesn't support this.
	if(Viewport->Actor->DefaultFOV!=customFOV)
	{		
		TCHAR buf[8]=L"fov ";
		_itow_s(customFOV,&buf[4],4,10);
		Viewport->Actor->DefaultFOV=customFOV; //Do this so the value is set even if FOV settings don't take effect (multiplayer mode) 
		URenderDevice::Viewport->Exec(buf,*GLog); //And this so the FOV change actually happens				
	}

	D3D::newFrame();

	//Set up flash if needed
	if( FlashScale!=FVector(.5,.5,.5) || FlashFog!=FVector(0,0,0) ) //From other renderers
	{		
		D3D::Vec4 color = {FlashFog.X,FlashFog.Y,FlashFog.Z,Min(FlashScale.X*2.f,1.f)}; //Min() formula from other renderers
		D3D::flash(1,color);
	}
	else
	{
		D3D::flash(0,*(D3D::Vec4*)NULL);
	}

	//Clear
	D3D::clearDepth();	// Depth needs to be always cleared
	D3D::clear(*((D3D::Vec4*)&ScreenClear.X));

	/*
	if(RenderLockFlags & LOCKR_ClearScreen)
	{		
			
	}
	*/

	//Lock
	D3D::map(true);	

	drawingWeapon=false;
}

/**
Finish rendering.
/param Blit Whether the front and back buffers should be swapped.
*/
void UD3D12RenderDevice::Unlock(UBOOL Blit)
{
	D3D::render();

	if(Blit)
	{
		D3D::present();
	}
}

/**
Complex surfaces are used for map geometry. They consists of facets which in turn consist of polys (triangle fans).
\param Frame The scene. See SetSceneNode().
\param Surface Holds information on the various texture passes and the surface's PolyFlags.
	- PolyFlags contains the correct flags for this surface. See polyflags.h
	- Texture is the diffuse texture.
	- DetailTexture is the nice close-up detail that's modulated with the diffuse texture for walls. It's up to the renderer to only draw these on near surfaces.
	- LightMap is the precalculated map lighting. Should be drawn with a -.5 pan offset.
	- FogMap is precalculated fog. Should be drawn with a -.5 pan offset. Should be added, not modulated. Flags determine if it should be applied, see polyflags.h.
	- MacroTexture is similar to a detail texture but for far away surfaces. Rarely used.
\param Facet Contains coordinates and polygons.
	- MapCoords are used to calculate texture coordinates. Involved. See code.
	- Polys is a linked list of triangle fan arrays; each element is similar to the models used in DrawGouraudPolygon().
	
\note DetailTexture and FogMap are mutually exclusive; D3D10 renderer just uses seperate binds for them anyway.
\note D3D10 renderer handles DetailTexture range in shader.
\note Check if submitted polygons are valid (3 or more points).
*/
void UD3D12RenderDevice::DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{	
	D3D::setProjectionMode(D3D::PROJ_NORMAL);


	D3D::setFlags(Surface.PolyFlags,0);

	//Cache and set textures
	D3D::TextureMetaData *diffuse=NULL, *lightMap=NULL, *detail=NULL, *fogMap=NULL, *macro=NULL;
	PrecacheTexture(*Surface.Texture,Surface.PolyFlags);	
	if(!(diffuse = D3D::setTexture(D3D::PASS_DIFFUSE,Surface.Texture->CacheID)))
		return;

	if(Surface.LightMap)
	{
		PrecacheTexture(*Surface.LightMap,0);
		if(!(lightMap = D3D::setTexture(D3D::PASS_LIGHT,Surface.LightMap->CacheID)))
			return;
	}
	else
	{
		D3D::setTexture(D3D::PASS_LIGHT,NULL);
	}
	if(Surface.DetailTexture)
	{
		PrecacheTexture(*Surface.DetailTexture,0);
		if(!(detail = D3D::setTexture(D3D::PASS_DETAIL,Surface.DetailTexture->CacheID)))
			return;
	}
	else
	{
		D3D::setTexture(D3D::PASS_DETAIL,NULL);
	}
	if(Surface.FogMap)
	{
		PrecacheTexture(*Surface.FogMap,0);
		if(!(fogMap = D3D::setTexture(D3D::PASS_FOG,Surface.FogMap->CacheID)))
			return;
	}
	else
	{
		D3D::setTexture(D3D::PASS_FOG,NULL);
	}
	if(Surface.MacroTexture)
	{
		PrecacheTexture(*Surface.MacroTexture,0);
		if(!(macro = D3D::setTexture(D3D::PASS_MACRO,Surface.MacroTexture->CacheID)))
			return;
	}
	else
	{
		macro = D3D::setTexture(D3D::PASS_MACRO,NULL);	
	}

	//Code from OpenGL renderer to calculate texture coordinates
	FLOAT UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
	FLOAT VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;
	
	//Draw each polygon
	for(FSavedPoly* Poly=Facet.Polys; Poly; Poly=Poly->Next )
	{
		if(Poly->NumPts < 3) //Skip invalid polygons
			continue;

		D3D::indexTriangleFan(Poly->NumPts); //Reserve space and generate indices for fan		
		for( INT i=0; i<Poly->NumPts; i++ )
		{
			D3D::Vertex *v = D3D::getVertex();
			
			//Code from OpenGL renderer to calculate texture coordinates
			FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
			FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
			FLOAT UCoord = U-UDot;
			FLOAT VCoord = V-VDot;

			//Diffuse texture coordinates
			v->TexCoord[0].x = (UCoord-Surface.Texture->Pan.X)*diffuse->multU;
			v->TexCoord[0].y = (VCoord-Surface.Texture->Pan.Y)*diffuse->multV;

			if(Surface.LightMap)
			{
				//Lightmaps require pan correction of -.5
				v->TexCoord[1].x = (UCoord-(Surface.LightMap->Pan.X-0.5f*Surface.LightMap->UScale) )*lightMap->multU; 
				v->TexCoord[1].y = (VCoord-(Surface.LightMap->Pan.Y-0.5f*Surface.LightMap->VScale) )*lightMap->multV;
			}
			if(Surface.DetailTexture)
			{
				v->TexCoord[2].x = (UCoord-Surface.DetailTexture->Pan.X)*detail->multU; 
				v->TexCoord[2].y = (VCoord-Surface.DetailTexture->Pan.Y)*detail->multV;
			}
			if(Surface.FogMap)
			{
				//Fogmaps require pan correction of -.5
				v->TexCoord[3].x = (UCoord-(Surface.FogMap->Pan.X-0.5f*Surface.FogMap->UScale) )*fogMap->multU; 
				v->TexCoord[3].y = (VCoord-(Surface.FogMap->Pan.Y-0.5f*Surface.FogMap->VScale) )*fogMap->multV;			
			}
			if(Surface.MacroTexture)
			{
				v->TexCoord[4].x = (UCoord-Surface.MacroTexture->Pan.X)*macro->multU; 
				v->TexCoord[4].y = (VCoord-Surface.MacroTexture->Pan.Y)*macro->multV;			
			}
			
			static D3D::Vec4 color = {1.0f,1.0f,1.0f,1.0f};
			v->Color = color; //No color as lighting comes from light maps (or is fullbright if none present)	
			v->flags = Surface.PolyFlags;
			v->Pos = *(D3D::Vec3*)&Poly->Pts[i]->Point.X; //Position

		}
	
	}

}

/**
Gouraud shaded polygons are used for 3D models and surprisingly shadows. 
They are sent with a call of this function per triangle fan, worldview transformed and lit. They do have normals and texture coordinates (no panning).
\param Frame The scene. See SetSceneNode().
\param Info The texture for the model. Models only come with diffuse textures.
\param Pts A triangle fan stored as an array. Each element has a normal, light (i.e. color) and fog (color due to being in fog).
\param NumPts Number of verts in fan.
\param PolyFlags Contains the correct flags for this model. See polyflags.h
\param Span Probably for software renderers.

\note Modulated models (i.e. shadows) shouldn't have a color, and fog should only be applied to models with the correct flags for that. The D3D10 renderer handles this in the shader.
\note Check if submitted polygons are valid (3 or more points).
*/
void UD3D12RenderDevice::DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span )
{
	
	if(NumPts<3) //Invalid triangle
		return;

	//Deus Ex clears the depth before drawing weapons; Unreal and UT don't. 
	//Detect them for those games and clear depth and draw shifted forwards (resulting in higher possible zNear).	
	#if(UNREALTOURNAMENT || UNREALGOLD)	
	if(!drawingWeapon)
	{
		if(Pts[0]->Point.Z<12)
		{							
			D3D::clearDepth();
			drawingWeapon=true;
		}
	}
	#endif

	if(!drawingWeapon)
		D3D::setProjectionMode(D3D::PROJ_NORMAL);
	else
		D3D::setProjectionMode(D3D::PROJ_COMPENSATE_Z_NEAR); //Have shader compensate w for moving

	//Set texture
	PrecacheTexture(Info,PolyFlags);
	D3D::TextureMetaData *diffuse = NULL;
	if(!(diffuse=D3D::setTexture(D3D::PASS_DIFFUSE,Info.CacheID)))
		return;
	D3D::setTexture(D3D::PASS_LIGHT,NULL);
	D3D::setTexture(D3D::PASS_DETAIL,NULL);
	D3D::setTexture(D3D::PASS_FOG,NULL);
	D3D::setTexture(D3D::PASS_MACRO,NULL);
	D3D::setFlags(PolyFlags,0);

	//Buffer triangle fans
	D3D::indexTriangleFan(NumPts); //Reserve space and generate indices for fan
	for(INT i=0; i<NumPts; i++) //Set fan verts
	{
		D3D::Vertex *v = D3D::getVertex();				
		v->Pos = *(D3D::Vec3*)&Pts[i]->Point.X;
		v->Normal = *(D3D::Vec3*)&Pts[i]->Normal.X;
		v->TexCoord[0].x = (Pts[i]->U)*diffuse->multU;
		v->TexCoord[0].y = (Pts[i]->V)*diffuse->multV;
		v->Color = *(D3D::Vec4*)&Pts[i]->Light.X;
		v->Fog = *(D3D::Vec4*)&Pts[i]->Fog.X;
		v->flags = PolyFlags;

		#ifdef RUNE
		if(PolyFlags & PF_AlphaBlend)
		{
			v->Color.w = (Info.Texture->Alpha);
		}
		#endif		
	}
}

/**
Used for 2D UI elements, coronas, etc. 
\param Frame The scene. See SetSceneNode().
\param Info The texture for the quad.
\param X X coord in screen space.
\param Y Y coord in screen space.
\param XL Width in pixels
\param YL Height in pixels
\param U Texure U coordinate for left.
\param V Texture V coordinate for top.
\param UL U+UL is coordinate for right.
\param VL V+VL is coordinate for bottom.
\param Span Probably for software renderers.
\param Z coordinate (similar to that of other primitives).
\param Color color
\param Fog fog
\param PolyFlags Contains the correct flags for this tile. See polyflags.h

\note Need to set scene node here otherwise Deus Ex dialogue letterboxes will look wrong; they aren't properly sent to SetSceneNode() it seems.
\note Drawn by converting pixel coordinates to -1,1 ranges in vertex shader and drawing quads with X/Y perspective transform disabled.
The Z coordinate however is transformed and divided by W; then W is set to 1 in the shader to get correct depth and yet preserve X and Y.
Other renderers take the opposite approach and multiply X by RProjZ*Z and Y by RProjZ*Z*aspect so they are preserved and then transform everything.
*/
void UD3D12RenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags )
{
	D3D::setProjectionMode(D3D::PROJ_Z_ONLY);
	SetSceneNode(Frame); //Set scene node fix.
	PrecacheTexture(Info,PolyFlags);
	D3D::TextureMetaData *diffuse = NULL;
	if(!(diffuse=D3D::setTexture(D3D::PASS_DIFFUSE,Info.CacheID)))
		return;
	D3D::setTexture(D3D::PASS_LIGHT,NULL);
	D3D::setTexture(D3D::PASS_DETAIL,NULL);
	D3D::setTexture(D3D::PASS_FOG,NULL);
	D3D::setTexture(D3D::PASS_MACRO,NULL);
	
	//if(Info.bRealtimeChanged) //DEUS EX: use this  to catch zyme, toxins etc
	{}

	D3D::setFlags(PolyFlags,0);
	D3D::indexQuad();

	float left = X;
	float right =X+XL;
	float top = Y;
	float bottom = Y+YL;

	float texLeft = U;
	float texRight = texLeft+UL;
	float texTop = V;
	float texBottom = texTop+VL;
	texLeft *= diffuse->multU; texRight *= diffuse->multU;
	texTop *= diffuse->multV; texBottom *= diffuse->multV;

	D3D::Vertex v;	
	v.Color = *((D3D::Vec4*)&Color.X);
	#ifdef RUNE
	if(PolyFlags & PF_AlphaBlend)
	{
		v.Color.w = (Info.Texture->Alpha);
	}
	#endif
	v.Fog = *((D3D::Vec4*)&Fog.X);
	
	
	v.Pos.z = Z;
	
	v.flags = PolyFlags;

	//Top left
	v.Pos.x = left;
	v.Pos.y = top;
	v.TexCoord[0].x = texLeft;
	v.TexCoord[0].y = texTop;
	*D3D::getVertex() = v;

	//Top right
	v.Pos.x = right;
	v.Pos.y = top;
	v.TexCoord[0].x = texRight;
	v.TexCoord[0].y = texTop;
	*D3D::getVertex() = v;

	//Bottom right
	v.Pos.x = right;
	v.Pos.y = bottom;
	v.TexCoord[0].x = texRight;
	v.TexCoord[0].y = texBottom;
	*D3D::getVertex() = v;

	//Bottom left
	v.Pos.x = left;
	v.Pos.y = bottom;
	v.TexCoord[0].x = texLeft;
	v.TexCoord[0].y = texBottom;
	*D3D::getVertex() = v;
}

/**
For UnrealED.
*/
void UD3D12RenderDevice::Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 )
{
}

/**
For UnrealED.
*/
void UD3D12RenderDevice::Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z )
{
}

/**
Clear the depth buffer. Used to draw the skybox behind the rest of the geometry, and weapon in front.
\note It is important that any vertex buffer contents be commited before actually clearing the depth!
*/
void UD3D12RenderDevice::ClearZ( FSceneNode* Frame )
{
	D3D::clearDepth();
}

/**
Something to do with clipping planes, not needed. 
*/
void UD3D12RenderDevice::PushHit( const BYTE* Data, INT Count )
{
}

/**
Something to do with clipping planes, not needed. 
*/
void UD3D12RenderDevice::PopHit( INT Count, UBOOL bForce )
{
}

/**
Something to do with FPS counters etc, not needed. 
*/
void UD3D12RenderDevice::GetStats( TCHAR* Result )
{

}

/**
Used for screenshots and savegame previews.
\param Pixels An array of 32 bit pixels in which to dump the back buffer.
*/
void UD3D12RenderDevice::ReadPixels( FColor* Pixels )
{
	UD3D12RenderDevice::debugs("Dumping screenshot...");
	D3D::getScreenshot((D3D::Vec4_byte*)Pixels);
	UD3D12RenderDevice::debugs("Done");
}

/**
Various command from the game. Can be used to intercept input. First let the parent class handle the command.

\param Cmd The command
	- GetRes Should return a list of resolutions in string form "HxW HxW" etc.
	- Brightness is intercepted here
\param Ar A class to which to log responses using Ar.Log().

\note Deus Ex ignores resolutions it does not like.
*/
UBOOL UD3D12RenderDevice::Exec(const TCHAR* Cmd, FOutputDevice& Ar)
{
	//First try parent
	wchar_t* ptr;
	#if (!UNREALGOLD)
	if(URenderDevice::Exec(Cmd,Ar))
	{
		return 1;
	}
	else
	#endif
	if(ParseCommand(&Cmd,L"GetRes"))
	{
		UD3D12RenderDevice::debugs("Getting modelist...");
		TCHAR* resolutions=D3D::getModes();
		Ar.Log(resolutions);
		delete [] resolutions;
		UD3D12RenderDevice::debugs("Done.");
		return 1;
	}	
	else if((ptr=(wchar_t*)wcswcs(Cmd,L"Brightness"))) //Brightness is sent as "brightness [val]".
	{
		UD3D12RenderDevice::debugs("Setting brightness.");
		if((ptr=wcschr(ptr,' ')))//Search for space after 'brightness'
		{
			float b;
			b=_wtof(ptr); //Get brightness value;
			D3D::setBrightness(b);
		}
	}
	return 0;
}



/**
This optional function can be used to set the frustum and viewport parameters per scene change instead of per drawXXXX() call.
\param Frame Contains various information with which to build frustum and viewport.
\note Standard Z parameters: near 1, far 32760. However, it seems ComplexSurfaces (except water's surface when in it) are at least at Z = ~13; models in DX cut scenes ~7. Can be utilized to gain increased z-buffer precision.
Unreal/UT weapons all seem to fall within ZWeapons: Z<12. Can be used to detect, clear depth (to prevent intersecting world) and move them. Only disadvantage of using increased zNear is that water surfaces the player is bobbing in don't look as good.
The D3D10 renderer moves gouraud polygons and tiles with Z < zNear (or Z < ZWeapons if needed) inside the range, allowing Unreal/UT weapons (after a depth clear) and tiles to be displayed correctly. ComplexSurfaces are not moved as this results in odd looking water surfaces.
*/
void UD3D12RenderDevice::SetSceneNode(FSceneNode* Frame )
{
	//Calculate projection parameters
	float aspect = Frame->FY/Frame->FX;
	float RProjZ = appTan(Viewport->Actor->FovAngle * PI/360.0 );

	D3D::setViewPort(Frame->X,Frame->Y,Frame->XB,Frame->YB); //Viewport is set here as it changes during gameplay. For example in DX conversations
	D3D::setProjection(aspect,RProjZ);		
}

/**
Store a texture in the renderer-kept texture cache. Only called by the game if URenderDevice::PrecacheOnFlip is 1.
\param Info Texture (meta)data. Includes a CacheID with which to index.
\param PolyFlags Contains the correct flags for this texture. See polyflags.h

\note Already cached textures are skipped, unless it's a dynamic texture, in which case it is updated.
\note Extra care is taken to recache textures that aren't saved as masked, but now have flags indicating they should be (masking is not always properly set).
	as this couldn't be anticipated in advance, the texture needs to be deleted and recreated.
*/
void UD3D12RenderDevice::PrecacheTexture( FTextureInfo& Info, DWORD PolyFlags )
{
	if(D3D::textureIsCached(Info.CacheID))
	{
		if(Info.bRealtimeChanged) //Update already cached realtime textures
		{
			TexConversion::update(Info,PolyFlags);
			return;
		}
		else if((PolyFlags & PF_Masked)&&!D3D::getTextureMetaData(Info.CacheID).masked) //Mask bit changed. Static texture, so must be deleted and recreated.
		{			
			D3D::deleteTexture(Info.CacheID);	
		}
		else //Texture is already cached and doesn't need to be modified
		{
			return;
		}		
	}

	//Cache texture
	TexConversion::convertAndCache(Info,PolyFlags); //Fills TextureInfo with metadata and a D3D format texture		

}

/**
Other renderers handle flashes here by saving the related structures; this one does it in Lock().
*/
void  UD3D12RenderDevice::EndFlash()
{

}

#ifdef RUNE
/**
Rune world fog is drawn by clearing the screen in the fog color, clipping the world geometry outside the view distance
and then overlaying alpha blended planes. Unfortunately this function is only called once it's actually time to draw the
fog, as such it's difficult to move this into a shader.

\param Frame The scene. See SetSceneNode().
\param ForSurf Fog plane information. Should be drawn with alpha blending enabled, color alpha = position.z/FogDistance.
\note The pre- and post function for this are meant to set blend state but aren't really needed.
*/
void UD3D12RenderDevice::DrawFogSurface(FSceneNode* Frame, FFogSurf &FogSurf)
{
	float mult = 1.0/FogSurf.FogDistance;
	D3D::setProjectionMode(D3D::PROJ_NORMAL);
	
	D3D::setFlags(PF_AlphaBlend,0);
	D3D::setTexture(D3D::PASS_DIFFUSE,NULL);
	D3D::setTexture(D3D::PASS_LIGHT,NULL);
	D3D::setTexture(D3D::PASS_DETAIL,NULL);
	D3D::setTexture(D3D::PASS_FOG,NULL);
	D3D::setTexture(D3D::PASS_MACRO,NULL);	
	for(FSavedPoly* Poly = FogSurf.Polys; Poly; Poly = Poly->Next)
	{
		D3D::indexTriangleFan(Poly->NumPts);
		for(int i=0; i<Poly->NumPts; i++ )
		{
			D3D::Vertex* v = D3D::getVertex();
			v->flags=FogSurf.PolyFlags;
			v->Color=*((D3D::Vec4*)&FogSurf.FogColor.X);			
			v->Pos = *(D3D::Vec3*)&Poly->Pts[i]->Point.X;
			v->Color.w = v->Pos.z*mult;		
			v->flags = PF_AlphaBlend;
		}
	}
}

/**
Rune object fog is normally drawn using the API's linear fog methods. In the D3D10 case, in the shader.
This function tells us how to configure the fog.

\param Frame The scene. See SetSceneNode().
\param FogDistance The end distance of the fog (start distance is always 0)
\param FogColor The fog's color.
*/
void UD3D12RenderDevice::PreDrawGouraud(FSceneNode *Frame, FLOAT FogDistance, FPlane FogColor)
{
	if(FogDistance>0)
	{
		D3D::Vec4 *color = ((D3D::Vec4*)&FogColor.X);
		D3D::fog(FogDistance,color);
	}
}

/**
Turn off fogging off.
\param FogDistance Distance with which fog was previously turned on.
*/
void UD3D12RenderDevice::PostDrawGouraud(FLOAT FogDistance)
{
	if(FogDistance>0)
	{
		D3D::fog(0,NULL);
	}
}
#endif
