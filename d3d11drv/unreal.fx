/**
\file unreal.fx

FX file for Unreal engine Direct3D 11 renderer.
Marijn Kentie 2009
*/

#include "unreal.fxh"

/** read MSAA pixel */
float4 directCopy(float2 coords, Texture2DMS<float4,SAMPLES> tex)
{
		float4 color=(float4)0;
		for(int i=0;i<SAMPLES;i++)
		{
			color += tex.Load(coords,i);		
		}
		color/=SAMPLES;
		return color;
}

#include "unreal_pom.fx"

//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
GS_INPUT VS( VS_INPUT input )
{
	GS_INPUT output = (GS_INPUT)0;

	/*
	Position
	*/
	//Projection transform. If requested, move vertices into (pushed forward) zNear.
	if(projectionMode==PROJ_Z_ONLY || projectionMode==PROJ_COMPENSATE_Z_NEAR)
		input.pos.z+=Z_NEAR-1;
	float4 projected=mul(input.pos,projection);
	if( projectionMode==PROJ_COMPENSATE_Z_NEAR)
		projected.w-=Z_NEAR-1;
	
	
	if(projectionMode==PROJ_Z_ONLY) //No projection except for Z (for screen space elements)
	{	
		//Convert coordinates to [-1,1] range
		output.pos.x=-1+(2*input.pos.x/viewportWidth);
		output.pos.y=-1+(2*input.pos.y/viewportHeight);
		//Use perspective transformed depth for correct occlusion
		output.pos.z = projected.z/projected.w;
		output.pos.w = 1; //W set to 1 so X and Y aren't divided.
	}
	else
	{		
		output.pos = projected;		
	}
	output.pos.z=clamp(output.pos.z,0,99999999); //ATI fix
	
	/*
	Color
	*/
	if(input.flags&PF_Modulated) //Modulated not influenced by color
	{
		output.color = float4(1,1,1,1);
	}
	else
	{
		output.color = clamp(input.color,0,1); //Color is sometimes >1 for Rune which screws up runestone particles
		if(!(input.flags&PF_AlphaBlend))
			output.color.a=1;
	}
	
	/*
	Fog
	*/
	//From OpenGL renderer; seems fog should not be combined with these other effects
	if((input.flags & (PF_RenderFog|PF_Translucent|PF_Modulated|PF_AlphaBlend))==PF_RenderFog)
	{
		output.fog = input.fog*FOG_SCALE;
	}
	else
	{
		output.fog=float4(0,0,0,0);
	}
	
	/*
	Textures
	*/
	for(int i=0;i<NUM_TEXTURE_PASSES;i++)
	{
		output.tex[i] = input.tex[i];
	}
	
	/*
	Misc
	*/
	output.origPos = input.pos;
	output.flags = input.flags;
	
	//d3d vs unreal coords
	output.pos.y =  -output.pos.y;
	output.origPos.y = -output.origPos.y;
	//output.normal.y = -output.normal.y;

	return output;
}


//--------------------------------------------------------------------------------------
// Geometry Shader
//--------------------------------------------------------------------------------------
[maxvertexcount(3)]
void GS( triangle GS_INPUT input[3], inout TriangleStream <PS_INPUT> triStream )
{
	PS_INPUT output = (PS_INPUT)0;
	
	float3 positions[3];
	float2 coords[3];
	
	int i;
	
	#if(POM_ENABLED==1)
	//Compute tangent space vectors for the triangle
	for(i =0; i<3; i++)
	{
		positions[i]=input[i].origPos;
		coords[i]=input[i].tex[0];
	}
	float3x3 tangentSpace = computeTangentSpaceMatrix(positions,coords);
	#endif
	
	for(i=0; i<3; i++ )
	{
		
		#if(POM_ENABLED==1)
		output.viewTS = mul(tangentSpace,-input[i].origPos);
		output.vParallaxOffsetTS = calcPOMVector(output.viewTS);
		output.normal = tangentSpace[2];
		#endif
		
		//Standard propagation
		output.pos = input[i].pos;
		output.fog = input[i].fog;
		for(int j=0;j<NUM_TEXTURE_PASSES;j++)
		{
			output.tex[j] = input[i].tex[j];
		}
		output.texCentroid=input[i].tex[0];
		output.flags = input[i].flags;
		output.origPos = input[i].origPos;
		output.color = input[i].color;
		
		
		triStream.Append(output);
	
	}
	triStream.RestartStrip();
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
PS_OUTPUT PS( PS_INPUT input)
{
	PS_OUTPUT output;
	//Initialize all textures to have no influence
	float4 color = input.color;
	float4 fog = input.fog;
	float4 diffuse = float4(1.0f,1.0f,1.0f,1.0f);
	float4 light = float4(1.0f,1.0f,1.0f,1.0f);
	float4 detail = float4(1.0f,1.0f,1.0f,1.0f);
	float4 fogmap = float4(0.0f,0.0f,0.0f,0.0f);
	float4 macro = float4(1.0f,1.0f,1.0f,1.0f);
		
	//Handle texture passes
	if(useTexturePass[0]) //Diffuse
	{
		diffuse = textures[0].SampleBias(sam,input.tex[0],LODBIAS);
		float4 diffusePoint = textures[0].SampleBias(samPoint,input.texCentroid,LODBIAS); //Centroid sampling for better behaviour with AA
		
			
		//Alpha test; point sample to get rid of seams
		if(input.flags&PF_Masked && !(input.flags&(PF_AlphaBlend|PF_Translucent)))
		{
			#if(!ALPHA_TO_COVERAGE_ENABLED)
			clip(diffusePoint.a-0.5f);
			clip(diffuse.a-0.5f);
			#endif
		}
		
		if(input.flags&PF_NoSmooth)
		{
			diffuse = diffusePoint;
		}
		else
		{
			//Sample skies a 2nd time for nice effect
			if(input.flags&PF_AutoUPan || input.flags&PF_AutoVPan) 
			{
				diffuse = .5*diffuse+.5*textures[0].SampleBias(sam,input.tex[0]*2,LODBIAS);
			}
		}
	
	}
	if(useTexturePass[1]) //Light
	{
		light = textures[1].SampleLevel(sam,input.tex[1],0);		
		light.rgba = light.bgra*2*LIGHT_SCALE; //Convert BGRA 7 bit to RGBA 8 bit	

	}
	if(useTexturePass[2]) //Detail (blend two detail texture samples with no detail for a nice effect).
	{
		//Interpolate between no detail and detail depending on how close the object is. Z=380 comes from UT D3D renderer.
		const int zFar = 380;
		float far = saturate(length(input.origPos)/zFar);
		if(far<1)
		{
			#if(POM_ENABLED==1)
			input.tex[2] = POM(input.origPos,input.viewTS,input.normal,input.tex[2],input.vParallaxOffsetTS,textures[2]);
			#endif
			detail = textures[2].SampleLevel(sam,input.tex[2],0);
			detail = lerp(detail,float4(1,1,1,1),far);
		}	
	}
	if(useTexturePass[3]) //Fog
	{		
		fogmap = textures[3].SampleLevel(sam,input.tex[3],0);				
		fogmap.rgba = fogmap.bgra*2*FOG_SCALE; //Convert BGRA 7 bit to RGBA 8 bit
	}
	if(useTexturePass[4]) //Macro
	{		
		macro = textures[4].SampleLevel(sam,input.tex[4],0);
	}
		
	output.color = color*diffuse*light*detail*macro+fogmap+fog;

	//Rune object fogging
	if(fogDist>0)
		output.color = lerp(output.color,fogColor,saturate(input.pos.w/fogDist));
		
	//Flash effect
	if(flashEnable && !(input.flags&(PF_Translucent|PF_Modulated)) && input.pos.z!=0) //Explosion, underwater, etc. effect; check z to skip UI elements
	{
		output.color += flashColor; //Ignore alpha as it messes up coverage to alpha AA.
	}
	
	//Brightness
	if(!(input.flags&PF_Modulated)) //Don't brighten DX mouse cursor, glasses, shadows, etc.
		output.color.rgb*=(.5+brightness);
	return output;
}


//--------------------------------------------------------------------------------------
technique11 Render
{
	pass Standard
	{
		SetVertexShader( CompileShader( vs_4_0, VS() ) );
		SetGeometryShader( CompileShader( gs_4_0, GS() ) );
		SetPixelShader( CompileShader( ps_4_0, PS() ) );
		
		SetRasterizerState(rstate_Default);             
		
		//These states are (unfortunately) set by the DLL
		
		//SetDepthStencilState(dstate_Enable,1.0);
		//SetBlendState(bstate_NoBlend,float4(0,0,0,0),0xffffffff);
	}
}