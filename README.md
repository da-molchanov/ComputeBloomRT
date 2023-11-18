# ComputeBloomRT
Takes a render target and blooms it in a compute shader.

Drop the `BP_BloomCapture` actor into the scene.
It would start capturing the scene into `RT_BloomCapture` every frame and writing the bloomed output into `RT_BloomCombined`.
Use `Materials/MI_ShowDownsample` and `Materials/MI_ShowUpsample` to visualize the mips of the render targets (best viewed on planes).

Use the `r.ComputeBloomRT.Radius` CVar (from 0 to 1) to vary the strength of the effect.

Make sure that the render targets have the same size and both have `Auto Generate Mips` checked.
You can supply your own render targets to the `ComputeBloomComponent` by changing the "Out Render Target" and "In Render Target" properties.
Use tick dependencies to ensure that the bloom compute shader invokation happens after you finish writing to the input render target.

Tested on standalone Meta Quest 2 with UE 4.27 Meta Fork.

## References
1. [https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/](https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/)
2. [https://medium.com/realities-io/using-compute-shaders-in-unreal-engine-4-f64bac65a907](https://medium.com/realities-io/using-compute-shaders-in-unreal-engine-4-f64bac65a907)
3. UE 4.27 Source Code: `TextureRenderTarget2D.cpp`, `GenerateMips.cpp`
