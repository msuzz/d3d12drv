/**
\file D3D11Drv.h
*/

#pragma once
#include "Engine.h"
#include "UnRender.h"
#include "d3d.h"
class UD3D11RenderDevice:public URenderDevice
{

//UObject glue
#if (UNREALTOURNAMENT || RUNE)
DECLARE_CLASS(UD3D11RenderDevice,URenderDevice,CLASS_Config,D3D11Drv)
#else
DECLARE_CLASS(UD3D11RenderDevice,URenderDevice,CLASS_Config)
#endif

private:
	D3D::Options D3DOptions;
	/** User configurable options */
	struct
	{
		int precache; /**< Turn on precaching */
	} options;

public:
	/**@name Helpers */
	//@{	
	static void debugs(char *s);
	int getOption(TCHAR* name,int defaultVal, bool isBool);
	//@}
	
	/**@name Abstract in parent class */
	//@{	
	UBOOL Init(UViewport *InViewport,INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen);
	UBOOL SetRes(INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen);
	void Exit();
	#if UNREALGOLD || UNREAL
	void Flush();	
	#else
	void Flush(UBOOL AllowPrecache);
	#endif
	void Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize );
	void Unlock(UBOOL Blit );
	void DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet );
	void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span );
	void DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, class FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags );
	void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 );
	void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z );
	void ClearZ( FSceneNode* Frame );
	void PushHit( const BYTE* Data, INT Count );
	void PopHit( INT Count, UBOOL bForce );
	void GetStats( TCHAR* Result );
	void ReadPixels( FColor* Pixels );
	//@}

	/**@name Optional but implemented*/
	//@{
	UBOOL Exec(const TCHAR* Cmd, FOutputDevice& Ar);
	void SetSceneNode( FSceneNode* Frame );
	void PrecacheTexture( FTextureInfo& Info, DWORD PolyFlags );
	void EndFlash();
	void StaticConstructor();
	//@}

	#ifdef RUNE
	/**@name Rune fog*/
	//@{
	void DrawFogSurface(FSceneNode* Frame, FFogSurf &FogSurf);
	void PreDrawGouraud(FSceneNode *Frame, FLOAT FogDistance, FPlane FogColor);
	void PostDrawGouraud(FLOAT FogDistance);
	//@}
	#endif
};