| Run | Adapter reported by QRhi | Glass | Runs | Median ms | p95 ms (median of runs; min to max) | p99 ms | Mean FPS | FPS at p95 | GPU ms median / p95 | Machine CPU busy % during orbit |
|-|-|-|-|-|-|-|-|-|-|-|
| A-d3d11-rtx3090 | NVIDIA GeForce RTX 3090 (D3D11) | on | 3 | 0.15 | 0.34 (0.26 to 0.34) | 0.76 | 5514 | 2976 | 0.13 / 0.13 | 19 to 20 |
| A-d3d11-uhd770 | Intel(R) UHD Graphics 770 (D3D11) | on | 3 | 3.70 | 4.73 (4.69 to 4.74) | 4.85 | 270 | 211 | 2.40 / 2.52 | 19 to 46 |
| A-d3d11-warp | Microsoft Basic Render Driver (D3D11) | on | 3 | 42.46 | 46.36 (45.40 to 50.47) | 47.15 | 23 | 22 | 42.31 / 44.63 | 66 to 75 |
| A-opengl-rtx3090 | NVIDIA GeForce RTX 3090 (OpenGL) | on | 3 | 0.21 | 0.34 (0.34 to 0.51) | 0.46 | 4435 | 2941 | 0.14 / 0.14 | 19 to 30 |
| B-opengl-rtx3090 | NVIDIA GeForce RTX 3090 (OpenGL) | on | 3 | 0.49 | 0.64 (0.62 to 0.90) | 0.79 | 2004 | 1570 | 0.38 / 0.68 | 18 to 23 |
| A-d3d11-rtx3090 | NVIDIA GeForce RTX 3090 (D3D11) | off | 3 | 0.14 | 0.39 (0.24 to 0.48) | 0.82 | 5931 | 2577 | 0.11 / 0.11 | 21 to 78 |
| A-d3d11-uhd770 | Intel(R) UHD Graphics 770 (D3D11) | off | 3 | 3.41 | 4.39 (3.57 to 4.40) | 4.47 | 293 | 228 | 2.13 / 2.26 | 20 to 28 |
| A-d3d11-warp | Microsoft Basic Render Driver (D3D11) | off | 3 | 39.80 | 42.56 (41.99 to 42.81) | 43.34 | 25 | 23 | 39.75 / 44.66 | 66 to 69 |
| A-opengl-rtx3090 | NVIDIA GeForce RTX 3090 (OpenGL) | off | 3 | 0.19 | 0.33 (0.32 to 0.59) | 0.46 | 4901 | 3058 | 0.12 / 0.12 | 19 to 44 |
| B-opengl-rtx3090 | NVIDIA GeForce RTX 3090 (OpenGL) | off | 3 | 0.32 | 0.50 (0.47 to 0.67) | 0.66 | 2900 | 2016 | 0.27 / 0.54 | 18 to 51 |

| Run | Glass | First frame ms (process start) | Pick idle median / p95 ms | Pick while rendering median / p95 ms | Picks correct | Resize median / p95 / max ms | Idle frames in 2 s |
|-|-|-|-|-|-|-|-|
| A-d3d11-rtx3090 | on | 338 | 1.6 / 2.3 | 1.7 / 2.2 | 120/120 | 0.14 / 0.55 / 3.7 | 0 |
| A-d3d11-uhd770 | on | 827 | 5.7 / 6.5 | 14.9 / 15.4 | 120/120 | 2.64 / 7.29 / 12.4 | 0 |
| A-d3d11-warp | on | 231 | 83.6 / 86.2 | 230.0 / 232.8 | 120/120 | 38.47 / 43.00 / 175.3 | 0 |
| A-opengl-rtx3090 | on | 476 | 1.6 / 2.1 | 1.7 / 2.0 | 120/120 | 0.22 / 0.44 / 3.4 | 0 |
| B-opengl-rtx3090 | on | 544 | 4.6 / 5.6 | 0.7 / 1.5 | 120/120 | 0.50 / 0.98 / 5.9 | 0 |
| A-d3d11-rtx3090 | off | 337 | 1.6 / 2.2 | 2.0 / 2.9 | 120/120 | 0.12 / 0.66 / 3.4 | 0 |
| A-d3d11-uhd770 | off | 829 | 5.5 / 6.0 | 13.5 / 13.8 | 120/120 | 2.49 / 6.95 / 11.5 | 0 |
| A-d3d11-warp | off | 219 | 79.9 / 82.9 | 221.3 / 225.8 | 120/120 | 38.29 / 41.67 / 184.4 | 0 |
| A-opengl-rtx3090 | off | 468 | 1.6 / 2.1 | 1.6 / 2.2 | 120/120 | 0.20 / 0.36 / 4.6 | 0 |
| B-opengl-rtx3090 | off | 501 | 4.5 / 5.5 | 1.7 / 2.2 | 120/120 | 0.33 / 0.55 / 4.1 | 0 |

| Extra run | Adapter | Glass | dpr | Target px | Median ms | p95 ms | Mean FPS | Picks correct |
|-|-|-|-|-|-|-|-|-|
| A-d3d11-uhd770-msaa4 | Intel(R) UHD Graphics 770 (D3D11) | on | 1.5 | 1920x1080 | 6.50 | 6.80 | 153 | - |
| A-d3d11-rtx3090-msaa4 | NVIDIA GeForce RTX 3090 (D3D11) | on | 1.5 | 1920x1080 | 0.25 | 1.08 | 2657 | - |
| B-opengl-rtx3090-ownfbo | NVIDIA GeForce RTX 3090 (OpenGL) | on | 1.5 | 1920x1080 | 0.52 | 0.78 | 1800 | - |
| B-opengl-rtx3090-ownfbo | NVIDIA GeForce RTX 3090 (OpenGL) | off | 1.5 | 1920x1080 | 0.34 | 0.55 | 2618 | - |
| A-d3d11-rtx3090-scale0.6667 | NVIDIA GeForce RTX 3090 (D3D11) | on | 1.0000499999999999 | 1920x1080 | 0.16 | 0.80 | 4640 | 40/40 |
| B-opengl-rtx3090-scale0.6667 | NVIDIA GeForce RTX 3090 (OpenGL) | on | 1.0000499999999999 | 1920x1080 | 0.47 | 0.63 | 2027 | 40/40 |
| A-d3d11-rtx3090-scale1 | NVIDIA GeForce RTX 3090 (D3D11) | on | 1.5 | 1920x1080 | 0.16 | 0.53 | 5027 | 40/40 |
| B-opengl-rtx3090-scale1 | NVIDIA GeForce RTX 3090 (OpenGL) | on | 1.5 | 1920x1080 | 0.47 | 0.69 | 1982 | 40/40 |
| A-d3d11-rtx3090-scale2 | NVIDIA GeForce RTX 3090 (D3D11) | on | 3 | 1920x1080 | 0.16 | 0.53 | 5005 | 40/40 |
| B-opengl-rtx3090-scale2 | NVIDIA GeForce RTX 3090 (OpenGL) | on | 3 | 1920x1080 | 0.51 | 0.72 | 1857 | 40/40 |
