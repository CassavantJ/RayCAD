| Curves | Case | Mode | Method | Steps | Median ms (range of run medians) | p95 ms (max) | Worst ms | Mismatch > 1e-9 | Max diff | Fallbacks | Failed | Same hash every run |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 50 | D1 | default | full, PlaneGCS System::solve (reference) | 300 | 8.243 (8.108 to 8.248) | 10.241 | 16.221 | 0 | 0 | 0 | 0 | yes |
| 50 | D1 | default | full, sparse | 300 | 0.751 (0.750 to 0.753) | 0.969 | 4.987 | 0 | 2.8e-14 | 0 | 0 | yes |
| 50 | D1 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.089 (0.087 to 0.092) | 0.111 | 0.347 | 0 | 1.3e-11 | 0 | 0 | yes |
| 50 | D1 | default | reduced, sparse + fallback | 300 | 0.058 (0.058 to 0.060) | 0.067 | 0.118 | 0 | 1.3e-11 | 0 | 0 | yes |
| 50 | D1 | re-init | full, PlaneGCS System::solve (reference) | 300 | 8.382 (8.370 to 8.455) | 9.210 | 24.000 | 0 | 0 | 0 | 0 | yes |
| 50 | D1 | re-init | full, sparse | 300 | 0.748 (0.742 to 0.750) | 0.846 | 1.890 | 0 | 8.9e-15 | 0 | 0 | yes |
| 50 | D1 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.345 (0.342 to 0.357) | 0.382 | 0.746 | 0 | 1.3e-11 | 0 | 0 | yes |
| 50 | D1 | re-init | reduced, sparse + fallback | 300 | 0.058 (0.058 to 0.058) | 0.063 | 1.571 | 0 | 1.3e-11 | 0 | 0 | yes |
| 50 | D2 | default | full, PlaneGCS System::solve (reference) | 300 | 14.774 (14.746 to 14.862) | 16.673 | 40.279 | 0 | 0 | 0 | 0 | yes |
| 50 | D2 | default | full, sparse | 300 | 1.563 (1.561 to 1.569) | 1.859 | 3.966 | 0 | 1e-13 | 0 | 0 | yes |
| 50 | D2 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.995 (0.994 to 1.006) | 1.123 | 1.680 | 0 | 6e-12 | 0 | 0 | yes |
| 50 | D2 | default | reduced, sparse + fallback | 300 | 0.188 (0.186 to 0.188) | 0.209 | 0.638 | 0 | 6e-12 | 0 | 0 | yes |
| 50 | D2 | re-init | full, PlaneGCS System::solve (reference) | 300 | 15.562 (15.435 to 15.597) | 17.695 | 22.132 | 0 | 0 | 0 | 0 | yes |
| 50 | D2 | re-init | full, sparse | 300 | 1.575 (1.557 to 1.584) | 1.807 | 2.993 | 0 | 2.8e-14 | 0 | 0 | yes |
| 50 | D2 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 1.261 (1.256 to 1.269) | 1.441 | 3.299 | 0 | 6e-12 | 0 | 0 | yes |
| 50 | D2 | re-init | reduced, sparse + fallback | 300 | 0.189 (0.188 to 0.192) | 0.371 | 1.711 | 0 | 6e-12 | 0 | 0 | yes |
| 50 | D3 | default | full, PlaneGCS System::solve (reference) | 300 | 1.725 (1.723 to 1.769) | 2.881 | 9.402 | 0 | 0 | 0 | 0 | yes |
| 50 | D3 | default | full, sparse | 300 | 0.236 (0.233 to 0.238) | 0.599 | 10.327 | 0 | 3.6e-15 | 0 | 0 | yes |
| 50 | D3 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.003 (0.003 to 0.003) | 0.005 | 0.009 | 0 | 4.6e-12 | 0 | 0 | yes |
| 50 | D3 | default | reduced, sparse + fallback | 300 | 0.002 (0.002 to 0.003) | 0.003 | 0.004 | 0 | 4.6e-12 | 0 | 0 | yes |
| 50 | D3 | re-init | full, PlaneGCS System::solve (reference) | 300 | 2.467 (2.431 to 2.474) | 2.951 | 8.467 | 0 | 0 | 0 | 0 | yes |
| 50 | D3 | re-init | full, sparse | 300 | 0.261 (0.258 to 0.264) | 0.555 | 1.565 | 0 | 3.6e-15 | 0 | 0 | yes |
| 50 | D3 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.003 (0.003 to 0.003) | 0.004 | 0.026 | 0 | 4.6e-12 | 0 | 0 | yes |
| 50 | D3 | re-init | reduced, sparse + fallback | 300 | 0.002 (0.002 to 0.002) | 0.003 | 0.004 | 0 | 4.6e-12 | 0 | 0 | yes |
| 50 | D4 | default | full, PlaneGCS System::solve (reference) | 300 | 7.367 (7.302 to 7.412) | 8.580 | 26.143 | 0 | 0 | 0 | 0 | yes |
| 50 | D4 | default | full, sparse | 300 | 0.729 (0.722 to 0.735) | 1.261 | 3.588 | 0 | 1.8e-14 | 0 | 0 | yes |
| 50 | D4 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.135 (0.132 to 0.135) | 0.157 | 0.974 | 0 | 1.9e-11 | 0 | 0 | yes |
| 50 | D4 | default | reduced, sparse + fallback | 300 | 0.090 (0.088 to 0.090) | 0.114 | 0.175 | 0 | 1.9e-11 | 0 | 0 | yes |
| 50 | D4 | re-init | full, PlaneGCS System::solve (reference) | 300 | 10.841 (8.016 to 19.805) | 78.097 | 87.948 | 0 | 0 | 0 | 0 | yes |
| 50 | D4 | re-init | full, sparse | 300 | 0.926 (0.736 to 1.151) | 2.719 | 12.422 | 0 | 5.3e-15 | 0 | 0 | yes |
| 50 | D4 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.572 (0.444 to 0.705) | 1.155 | 11.198 | 0 | 1.9e-11 | 0 | 0 | yes |
| 50 | D4 | re-init | reduced, sparse + fallback | 300 | 0.091 (0.070 to 0.114) | 0.136 | 3.008 | 0 | 1.9e-11 | 0 | 0 | yes |
| 50 | D5 | default | full, PlaneGCS System::solve (reference) | 300 | 0.214 (0.214 to 0.410) | 0.493 | 64.874 | 0 | 0 | 0 | 0 | yes |
| 50 | D5 | default | full, sparse | 300 | 0.116 (0.116 to 0.220) | 0.263 | 0.562 | 0 | 7.1e-15 | 0 | 0 | yes |
| 50 | D5 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.019 (0.019 to 0.038) | 0.045 | 0.060 | 0 | 4.6e-11 | 0 | 0 | yes |
| 50 | D5 | default | reduced, sparse + fallback | 300 | 0.015 (0.015 to 0.029) | 0.031 | 0.049 | 0 | 4.6e-11 | 0 | 0 | yes |
| 50 | D5 | re-init | full, PlaneGCS System::solve (reference) | 300 | 0.905 (0.862 to 1.466) | 27.642 | 62.014 | 0 | 0 | 0 | 0 | yes |
| 50 | D5 | re-init | full, sparse | 300 | 0.133 (0.130 to 0.245) | 0.275 | 0.519 | 0 | 7.1e-15 | 0 | 0 | yes |
| 50 | D5 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.048 (0.046 to 0.088) | 0.109 | 1.402 | 0 | 4.6e-11 | 0 | 0 | yes |
| 50 | D5 | re-init | reduced, sparse + fallback | 300 | 0.018 (0.018 to 0.030) | 0.051 | 64.527 | 0 | 4.6e-11 | 0 | 0 | yes |
| 50 | D6 | default | full, PlaneGCS System::solve (reference) | 300 | 7.615 (7.561 to 18.298) | 53.509 | 101.080 | 0 | 0 | 0 | 0 | yes |
| 50 | D6 | default | full, sparse | 300 | 0.747 (0.741 to 1.200) | 2.698 | 33.339 | 0 | 2.8e-14 | 0 | 0 | yes |
| 50 | D6 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.091 (0.090 to 0.149) | 0.179 | 24.352 | 0 | 1.3e-11 | 0 | 0 | yes |
| 50 | D6 | default | reduced, sparse + fallback | 300 | 0.060 (0.059 to 0.093) | 0.123 | 0.368 | 0 | 1.3e-11 | 0 | 0 | yes |
| 50 | D6 | re-init | full, PlaneGCS System::solve (reference) | 300 | 8.550 (8.480 to 28.838) | 52.618 | 102.148 | 0 | 0 | 0 | 0 | yes |
| 50 | D6 | re-init | full, sparse | 300 | 0.787 (0.748 to 1.168) | 13.249 | 39.412 | 0 | 8.9e-15 | 0 | 0 | yes |
| 50 | D6 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.365 (0.351 to 0.548) | 0.635 | 30.729 | 0 | 1.3e-11 | 0 | 0 | yes |
| 50 | D6 | re-init | reduced, sparse + fallback | 300 | 0.060 (0.059 to 0.096) | 0.111 | 0.335 | 0 | 1.3e-11 | 0 | 0 | yes |
| 100 | D1 | default | full, PlaneGCS System::solve (reference) | 300 | 113.776 (67.003 to 226.811) | 313.009 | 334.236 | 0 | 0 | 0 | 0 | yes |
| 100 | D1 | default | full, sparse | 300 | 3.162 (2.234 to 3.780) | 27.434 | 37.427 | 0 | 1.2e-10 | 0 | 0 | yes |
| 100 | D1 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.696 (0.438 to 0.742) | 16.807 | 33.072 | 0 | 2.6e-11 | 0 | 0 | yes |
| 100 | D1 | default | reduced, sparse + fallback | 300 | 0.168 (0.103 to 0.181) | 0.256 | 32.552 | 0 | 2.6e-11 | 0 | 0 | yes |
| 100 | D1 | re-init | full, PlaneGCS System::solve (reference) | 300 | 111.114 (56.440 to 127.819) | 233.137 | 264.409 | 0 | 0 | 0 | 0 | yes |
| 100 | D1 | re-init | full, sparse | 300 | 2.523 (1.625 to 2.645) | 18.744 | 34.197 | 0 | 7.2e-12 | 0 | 0 | yes |
| 100 | D1 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 1.276 (0.801 to 1.351) | 4.916 | 21.463 | 0 | 5.7e-14 | 0 | 0 | yes |
| 100 | D1 | re-init | reduced, sparse + fallback | 300 | 0.175 (0.103 to 0.190) | 0.280 | 3.652 | 0 | 5.7e-14 | 0 | 0 | yes |
| 100 | D2 | default | full, PlaneGCS System::solve (reference) | 300 | 60.355 (51.932 to 96.815) | 120.576 | 166.039 | 0 | 0 | 0 | 0 | yes |
| 100 | D2 | default | full, sparse | 300 | 1.771 (1.595 to 2.501) | 3.816 | 13.890 | 0 | 2.1e-14 | 0 | 0 | yes |
| 100 | D2 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.215 (0.188 to 0.327) | 0.454 | 6.585 | 0 | 5.7e-14 | 0 | 0 | yes |
| 100 | D2 | default | reduced, sparse + fallback | 300 | 0.093 (0.089 to 0.146) | 0.203 | 0.523 | 0 | 5.7e-14 | 0 | 0 | yes |
| 100 | D2 | re-init | full, PlaneGCS System::solve (reference) | 300 | 54.104 (53.580 to 97.462) | 151.998 | 206.189 | 0 | 0 | 0 | 0 | yes |
| 100 | D2 | re-init | full, sparse | 300 | 1.696 (1.682 to 2.511) | 9.680 | 16.235 | 0 | 2.5e-14 | 0 | 0 | yes |
| 100 | D2 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.558 (0.555 to 0.850) | 2.563 | 6.920 | 0 | 5.7e-14 | 0 | 0 | yes |
| 100 | D2 | re-init | reduced, sparse + fallback | 300 | 0.100 (0.096 to 0.141) | 0.459 | 7.476 | 0 | 5.7e-14 | 0 | 0 | yes |
| 100 | D3 | default | full, PlaneGCS System::solve (reference) | 300 | 11.375 (11.247 to 21.986) | 36.976 | 45.404 | 0 | 0 | 0 | 0 | yes |
| 100 | D3 | default | full, sparse | 300 | 0.556 (0.553 to 0.622) | 5.049 | 9.373 | 0 | 3.6e-15 | 0 | 0 | yes |
| 100 | D3 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.008 (0.008 to 0.013) | 0.021 | 0.103 | 0 | 3.6e-15 | 0 | 0 | yes |
| 100 | D3 | default | reduced, sparse + fallback | 300 | 0.004 (0.004 to 0.004) | 0.005 | 0.400 | 0 | 3.6e-15 | 0 | 0 | yes |
| 100 | D3 | re-init | full, PlaneGCS System::solve (reference) | 300 | 13.225 (13.190 to 14.056) | 26.684 | 38.453 | 0 | 0 | 0 | 0 | yes |
| 100 | D3 | re-init | full, sparse | 300 | 0.567 (0.565 to 0.567) | 0.779 | 3.651 | 0 | 3.6e-15 | 0 | 0 | yes |
| 100 | D3 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.009 (0.008 to 0.009) | 0.013 | 0.021 | 0 | 3.6e-15 | 0 | 0 | yes |
| 100 | D3 | re-init | reduced, sparse + fallback | 300 | 0.004 (0.004 to 0.004) | 0.004 | 0.035 | 0 | 3.6e-15 | 0 | 0 | yes |
| 100 | D4 | default | full, PlaneGCS System::solve (reference) | 300 | 137.462 (136.208 to 141.267) | 172.923 | 218.039 | 0 | 0 | 0 | 0 | yes |
| 100 | D4 | default | full, sparse | 300 | 5.136 (4.957 to 5.429) | 7.412 | 14.059 | 0 | 2e-11 | 0 | 0 | yes |
| 100 | D4 | default | reduced, PlaneGCS System::solve + fallback | 300 | 10.423 (10.361 to 10.553) | 13.028 | 30.506 | 0 | 4.8e-13 | 0 | 0 | yes |
| 100 | D4 | default | reduced, sparse + fallback | 300 | 0.776 (0.771 to 0.831) | 1.786 | 3.898 | 0 | 2e-11 | 0 | 0 | yes |
| 100 | D4 | re-init | full, PlaneGCS System::solve (reference) | 300 | 127.276 (127.016 to 145.014) | 204.814 | 252.964 | 0 | 0 | 0 | 0 | yes |
| 100 | D4 | re-init | full, sparse | 300 | 4.327 (4.302 to 4.349) | 8.948 | 19.107 | 0 | 6.7e-13 | 0 | 0 | yes |
| 100 | D4 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 10.412 (10.207 to 10.600) | 18.123 | 41.553 | 0 | 1.1e-14 | 0 | 0 | yes |
| 100 | D4 | re-init | reduced, sparse + fallback | 300 | 0.694 (0.676 to 0.702) | 1.334 | 6.655 | 0 | 6.7e-13 | 0 | 0 | yes |
| 100 | D5 | default | full, PlaneGCS System::solve (reference) | 300 | 0.251 (0.246 to 0.258) | 1.319 | 4.856 | 0 | 0 | 0 | 0 | yes |
| 100 | D5 | default | full, sparse | 300 | 0.160 (0.156 to 0.167) | 0.539 | 3.378 | 0 | 7.1e-15 | 0 | 0 | yes |
| 100 | D5 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.250 (0.248 to 0.260) | 0.545 | 4.855 | 0 | 0 | 300 | 0 | yes |
| 100 | D5 | default | reduced, sparse + fallback | 300 | 0.161 (0.156 to 0.166) | 0.332 | 1.989 | 0 | 7.1e-15 | 300 | 0 | yes |
| 100 | D5 | re-init | full, PlaneGCS System::solve (reference) | 300 | 1.543 (1.524 to 1.758) | 5.654 | 7.596 | 0 | 0 | 0 | 0 | yes |
| 100 | D5 | re-init | full, sparse | 300 | 0.181 (0.178 to 0.202) | 2.819 | 7.129 | 0 | 7.1e-15 | 0 | 0 | yes |
| 100 | D5 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 1.538 (1.516 to 1.837) | 6.418 | 9.483 | 0 | 0 | 300 | 0 | yes |
| 100 | D5 | re-init | reduced, sparse + fallback | 300 | 0.181 (0.178 to 0.202) | 1.585 | 5.190 | 0 | 7.1e-15 | 300 | 0 | yes |
| 100 | D6 | default | full, PlaneGCS System::solve (reference) | 300 | 67.376 (65.385 to 68.690) | 105.850 | 134.939 | 0 | 0 | 0 | 0 | yes |
| 100 | D6 | default | full, sparse | 300 | 2.093 (2.068 to 2.345) | 5.582 | 12.193 | 0 | 1.2e-10 | 0 | 0 | yes |
| 100 | D6 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.437 (0.435 to 0.440) | 0.594 | 2.672 | 0 | 2.6e-11 | 0 | 0 | yes |
| 100 | D6 | default | reduced, sparse + fallback | 300 | 0.104 (0.103 to 0.104) | 0.166 | 1.340 | 0 | 2.6e-11 | 0 | 0 | yes |
| 100 | D6 | re-init | full, PlaneGCS System::solve (reference) | 300 | 60.014 (55.686 to 63.242) | 96.029 | 138.438 | 0 | 0 | 0 | 0 | yes |
| 100 | D6 | re-init | full, sparse | 300 | 1.673 (1.652 to 1.697) | 3.780 | 7.590 | 0 | 7.2e-12 | 0 | 0 | yes |
| 100 | D6 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.802 (0.785 to 0.821) | 3.027 | 4.918 | 0 | 5.7e-14 | 0 | 0 | yes |
| 100 | D6 | re-init | reduced, sparse + fallback | 300 | 0.104 (0.101 to 0.106) | 0.254 | 2.031 | 0 | 5.7e-14 | 0 | 0 | yes |
| 150 | D1 | default | full, PlaneGCS System::solve (reference) | 300 | 176.955 (175.511 to 205.232) | 294.562 | 438.746 | 0 | 0 | 0 | 0 | yes |
| 150 | D1 | default | full, sparse | 300 | 2.586 (2.564 to 2.663) | 8.425 | 23.785 | 0 | 2.8e-14 | 0 | 0 | yes |
| 150 | D1 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.135 (0.135 to 0.136) | 0.253 | 7.938 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D1 | default | reduced, sparse + fallback | 300 | 0.088 (0.088 to 0.088) | 0.147 | 0.505 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D1 | re-init | full, PlaneGCS System::solve (reference) | 300 | 180.852 (175.707 to 191.598) | 744.919 | 791.378 | 0 | 0 | 0 | 0 | yes |
| 150 | D1 | re-init | full, sparse | 300 | 2.564 (2.554 to 2.613) | 34.598 | 49.724 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D1 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.573 (0.563 to 0.580) | 1.312 | 37.089 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D1 | re-init | reduced, sparse + fallback | 300 | 0.088 (0.087 to 0.089) | 0.161 | 2.330 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D2 | default | full, PlaneGCS System::solve (reference) | 300 | 172.161 (171.308 to 316.080) | 693.543 | 773.779 | 0 | 0 | 0 | 0 | yes |
| 150 | D2 | default | full, sparse | 300 | 2.668 (2.589 to 4.162) | 21.857 | 36.563 | 0 | 3.6e-14 | 0 | 0 | yes |
| 150 | D2 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.208 (0.207 to 0.336) | 0.445 | 1.113 | 0 | 3.2e-14 | 0 | 0 | yes |
| 150 | D2 | default | reduced, sparse + fallback | 300 | 0.101 (0.101 to 0.161) | 0.258 | 0.480 | 0 | 4.3e-14 | 0 | 0 | yes |
| 150 | D2 | re-init | full, PlaneGCS System::solve (reference) | 300 | 174.310 (173.572 to 175.182) | 229.886 | 253.691 | 0 | 0 | 0 | 0 | yes |
| 150 | D2 | re-init | full, sparse | 300 | 3.579 (3.568 to 3.584) | 4.595 | 13.482 | 0 | 2.1e-14 | 0 | 0 | yes |
| 150 | D2 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.657 (0.648 to 0.664) | 0.854 | 1.543 | 0 | 2.8e-14 | 0 | 0 | yes |
| 150 | D2 | re-init | reduced, sparse + fallback | 300 | 0.105 (0.105 to 0.107) | 0.179 | 1.873 | 0 | 2.5e-14 | 0 | 0 | yes |
| 150 | D3 | default | full, PlaneGCS System::solve (reference) | 300 | 35.904 (35.414 to 37.031) | 48.194 | 59.142 | 0 | 0 | 0 | 0 | yes |
| 150 | D3 | default | full, sparse | 300 | 0.869 (0.858 to 0.880) | 1.696 | 6.502 | 0 | 3.6e-15 | 0 | 0 | yes |
| 150 | D3 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.011 (0.011 to 0.011) | 0.016 | 0.044 | 0 | 3.6e-15 | 0 | 0 | yes |
| 150 | D3 | default | reduced, sparse + fallback | 300 | 0.006 (0.006 to 0.006) | 0.007 | 0.009 | 0 | 3.6e-15 | 0 | 0 | yes |
| 150 | D3 | re-init | full, PlaneGCS System::solve (reference) | 300 | 37.911 (37.789 to 38.318) | 66.284 | 86.847 | 0 | 0 | 0 | 0 | yes |
| 150 | D3 | re-init | full, sparse | 300 | 0.873 (0.859 to 0.878) | 2.454 | 8.233 | 0 | 3.6e-15 | 0 | 0 | yes |
| 150 | D3 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.011 (0.011 to 0.012) | 0.014 | 0.035 | 0 | 3.6e-15 | 0 | 0 | yes |
| 150 | D3 | re-init | reduced, sparse + fallback | 300 | 0.006 (0.006 to 0.006) | 0.007 | 0.021 | 0 | 3.6e-15 | 0 | 0 | yes |
| 150 | D4 | default | full, PlaneGCS System::solve (reference) | 300 | 460.231 (446.882 to 476.659) | 1112.389 | 1675.950 | 0 | 0 | 0 | 0 | yes |
| 150 | D4 | default | full, sparse | 300 | 7.307 (7.222 to 7.530) | 23.381 | 37.754 | 0 | 2.3e-13 | 0 | 0 | yes |
| 150 | D4 | default | reduced, PlaneGCS System::solve + fallback | 300 | 7.054 (7.003 to 7.119) | 12.866 | 44.961 | 0 | 3.4e-13 | 0 | 0 | yes |
| 150 | D4 | default | reduced, sparse + fallback | 300 | 0.802 (0.791 to 0.806) | 1.699 | 17.596 | 0 | 3.4e-13 | 0 | 0 | yes |
| 150 | D4 | re-init | full, PlaneGCS System::solve (reference) | 300 | 431.503 (407.487 to 508.440) | 621.825 | 724.478 | 0 | 0 | 0 | 0 | yes |
| 150 | D4 | re-init | full, sparse | 300 | 6.581 (6.481 to 6.946) | 14.129 | 16.740 | 0 | 4.4e-16 | 0 | 0 | yes |
| 150 | D4 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 7.820 (7.572 to 8.535) | 15.730 | 21.841 | 0 | 3.9e-14 | 0 | 0 | yes |
| 150 | D4 | re-init | reduced, sparse + fallback | 300 | 0.741 (0.732 to 0.750) | 1.164 | 6.889 | 0 | 3.9e-14 | 0 | 0 | yes |
| 150 | D5 | default | full, PlaneGCS System::solve (reference) | 300 | 0.303 (0.303 to 0.306) | 0.375 | 1.532 | 0 | 0 | 0 | 0 | yes |
| 150 | D5 | default | full, sparse | 300 | 0.207 (0.206 to 0.207) | 0.237 | 0.642 | 0 | 1.3e-15 | 0 | 0 | yes |
| 150 | D5 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.024 (0.023 to 0.024) | 0.028 | 0.039 | 0 | 8.9e-16 | 0 | 0 | yes |
| 150 | D5 | default | reduced, sparse + fallback | 300 | 0.019 (0.019 to 0.019) | 0.023 | 0.062 | 0 | 8.9e-16 | 0 | 0 | yes |
| 150 | D5 | re-init | full, PlaneGCS System::solve (reference) | 300 | 2.141 (2.119 to 2.290) | 7.871 | 12.502 | 0 | 0 | 0 | 0 | yes |
| 150 | D5 | re-init | full, sparse | 300 | 0.234 (0.229 to 0.251) | 0.353 | 0.883 | 0 | 2.5e-15 | 0 | 0 | yes |
| 150 | D5 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.054 (0.053 to 0.061) | 0.084 | 1.199 | 0 | 2.8e-18 | 0 | 0 | yes |
| 150 | D5 | re-init | reduced, sparse + fallback | 300 | 0.022 (0.022 to 0.023) | 0.028 | 0.960 | 0 | 2.8e-18 | 0 | 0 | yes |
| 150 | D6 | default | full, PlaneGCS System::solve (reference) | 300 | 172.734 (169.536 to 176.487) | 252.155 | 468.473 | 0 | 0 | 0 | 0 | yes |
| 150 | D6 | default | full, sparse | 300 | 2.569 (2.558 to 2.592) | 4.776 | 6.559 | 0 | 2.8e-14 | 0 | 0 | yes |
| 150 | D6 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.137 (0.136 to 0.141) | 0.182 | 1.946 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D6 | default | reduced, sparse + fallback | 300 | 0.087 (0.087 to 0.087) | 0.114 | 0.554 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D6 | re-init | full, PlaneGCS System::solve (reference) | 300 | 178.274 (178.114 to 181.050) | 338.302 | 398.654 | 0 | 0 | 0 | 0 | yes |
| 150 | D6 | re-init | full, sparse | 300 | 2.622 (2.561 to 2.655) | 6.890 | 8.818 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D6 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.575 (0.573 to 0.577) | 0.891 | 3.392 | 0 | 7.1e-15 | 0 | 0 | yes |
| 150 | D6 | re-init | reduced, sparse + fallback | 300 | 0.089 (0.088 to 0.090) | 0.135 | 1.314 | 0 | 7.1e-15 | 0 | 0 | yes |
| 200 | D1 | default | full, PlaneGCS System::solve (reference) | 300 | 493.430 (490.477 to 535.730) | 1116.750 | 1860.221 | 0 | 0 | 0 | 0 | yes |
| 200 | D1 | default | full, sparse | 300 | 4.556 (4.482 to 4.572) | 13.726 | 27.983 | 0 | 1.4e-14 | 0 | 0 | yes |
| 200 | D1 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.134 (0.133 to 0.138) | 0.259 | 2.046 | 0 | 5.3e-15 | 0 | 0 | yes |
| 200 | D1 | default | reduced, sparse + fallback | 300 | 0.088 (0.086 to 0.088) | 0.160 | 30.416 | 0 | 5.3e-15 | 0 | 0 | yes |
| 200 | D1 | re-init | full, PlaneGCS System::solve (reference) | 300 | 404.119 (399.234 to 408.326) | 581.978 | 709.247 | 0 | 0 | 0 | 0 | yes |
| 200 | D1 | re-init | full, sparse | 300 | 3.454 (3.439 to 3.494) | 7.006 | 13.401 | 0 | 1.4e-14 | 0 | 0 | yes |
| 200 | D1 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.529 (0.518 to 0.537) | 0.956 | 2.542 | 0 | 2.1e-14 | 0 | 0 | yes |
| 200 | D1 | re-init | reduced, sparse + fallback | 300 | 0.089 (0.089 to 0.090) | 0.110 | 0.232 | 0 | 2.1e-14 | 0 | 0 | yes |
| 200 | D2 | default | full, PlaneGCS System::solve (reference) | 300 | 399.812 (397.681 to 439.306) | 1012.496 | 1562.216 | 0 | 0 | 0 | 0 | yes |
| 200 | D2 | default | full, sparse | 300 | 3.496 (3.458 to 3.535) | 9.593 | 41.403 | 0 | 3.2e-14 | 0 | 0 | yes |
| 200 | D2 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.209 (0.208 to 0.212) | 0.425 | 1.953 | 0 | 3.2e-14 | 0 | 0 | yes |
| 200 | D2 | default | reduced, sparse + fallback | 300 | 0.101 (0.100 to 0.102) | 0.193 | 0.290 | 0 | 4.3e-14 | 0 | 0 | yes |
| 200 | D2 | re-init | full, PlaneGCS System::solve (reference) | 300 | 425.875 (407.734 to 443.862) | 1445.187 | 1814.797 | 0 | 0 | 0 | 0 | yes |
| 200 | D2 | re-init | full, sparse | 300 | 5.069 (5.033 to 5.082) | 9.725 | 43.109 | 0 | 1.8e-14 | 0 | 0 | yes |
| 200 | D2 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.631 (0.614 to 0.640) | 1.354 | 33.259 | 0 | 2.8e-14 | 0 | 0 | yes |
| 200 | D2 | re-init | reduced, sparse + fallback | 300 | 0.129 (0.124 to 0.133) | 0.255 | 2.486 | 0 | 2.8e-14 | 0 | 0 | yes |
| 200 | D3 | default | full, PlaneGCS System::solve (reference) | 300 | 82.677 (81.202 to 86.290) | 113.438 | 144.803 | 0 | 0 | 0 | 0 | yes |
| 200 | D3 | default | full, sparse | 300 | 1.280 (1.245 to 1.285) | 2.616 | 5.496 | 0 | 3.6e-15 | 0 | 0 | yes |
| 200 | D3 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.015 (0.015 to 0.015) | 0.022 | 0.029 | 0 | 3.6e-15 | 0 | 0 | yes |
| 200 | D3 | default | reduced, sparse + fallback | 300 | 0.008 (0.008 to 0.008) | 0.009 | 0.162 | 0 | 3.6e-15 | 0 | 0 | yes |
| 200 | D3 | re-init | full, PlaneGCS System::solve (reference) | 300 | 89.614 (85.045 to 115.211) | 181.773 | 189.791 | 0 | 0 | 0 | 0 | yes |
| 200 | D3 | re-init | full, sparse | 300 | 1.282 (1.260 to 1.329) | 4.687 | 15.097 | 0 | 3.6e-15 | 0 | 0 | yes |
| 200 | D3 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.015 (0.015 to 0.017) | 0.030 | 0.378 | 0 | 3.6e-15 | 0 | 0 | yes |
| 200 | D3 | re-init | reduced, sparse + fallback | 300 | 0.008 (0.008 to 0.008) | 0.013 | 0.141 | 0 | 3.6e-15 | 0 | 0 | yes |
| 200 | D4 | default | full, PlaneGCS System::solve (reference) | 300 | 1062.033 (1041.090 to 1097.719) | 1325.850 | 2178.102 | 0 | 0 | 0 | 0 | yes |
| 200 | D4 | default | full, sparse | 300 | 10.920 (10.500 to 11.292) | 18.200 | 31.061 | 0 | 2e-11 | 0 | 0 | yes |
| 200 | D4 | default | reduced, PlaneGCS System::solve + fallback | 300 | 45.052 (44.395 to 46.237) | 67.907 | 106.938 | 0 | 6.2e-13 | 0 | 0 | yes |
| 200 | D4 | default | reduced, sparse + fallback | 300 | 1.462 (1.457 to 1.475) | 4.279 | 10.820 | 0 | 2e-11 | 0 | 0 | yes |
| 200 | D4 | re-init | full, PlaneGCS System::solve (reference) | 300 | 976.514 (958.263 to 1014.971) | 1249.864 | 1449.490 | 0 | 0 | 0 | 0 | yes |
| 200 | D4 | re-init | full, sparse | 300 | 9.252 (9.099 to 9.450) | 15.145 | 27.743 | 0 | 5.5e-13 | 0 | 0 | yes |
| 200 | D4 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 43.029 (42.763 to 43.934) | 69.842 | 87.859 | 0 | 2.8e-13 | 0 | 0 | yes |
| 200 | D4 | re-init | reduced, sparse + fallback | 300 | 1.292 (1.277 to 1.295) | 2.251 | 12.558 | 0 | 5.5e-13 | 0 | 0 | yes |
| 200 | D5 | default | full, PlaneGCS System::solve (reference) | 300 | 0.342 (0.342 to 0.343) | 0.414 | 2.229 | 0 | 0 | 0 | 0 | yes |
| 200 | D5 | default | full, sparse | 300 | 0.253 (0.252 to 0.263) | 0.299 | 0.602 | 0 | 6.9e-15 | 0 | 0 | yes |
| 200 | D5 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.343 (0.343 to 0.343) | 0.408 | 2.578 | 0 | 0 | 300 | 0 | yes |
| 200 | D5 | default | reduced, sparse + fallback | 300 | 0.253 (0.251 to 0.264) | 0.281 | 0.699 | 0 | 6.9e-15 | 300 | 0 | yes |
| 200 | D5 | re-init | full, PlaneGCS System::solve (reference) | 300 | 2.786 (2.731 to 2.804) | 5.200 | 16.045 | 0 | 0 | 0 | 0 | yes |
| 200 | D5 | re-init | full, sparse | 300 | 0.287 (0.283 to 0.288) | 0.369 | 1.887 | 0 | 1.4e-14 | 0 | 0 | yes |
| 200 | D5 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 2.805 (2.743 to 3.082) | 4.984 | 7.545 | 0 | 0 | 300 | 0 | yes |
| 200 | D5 | re-init | reduced, sparse + fallback | 300 | 0.288 (0.283 to 0.288) | 1.025 | 3.918 | 0 | 1.4e-14 | 300 | 0 | yes |
| 200 | D6 | default | full, PlaneGCS System::solve (reference) | 300 | 491.825 (482.526 to 505.529) | 640.480 | 788.625 | 0 | 0 | 0 | 0 | yes |
| 200 | D6 | default | full, sparse | 300 | 4.507 (4.439 to 4.531) | 6.754 | 14.952 | 0 | 1.4e-14 | 0 | 0 | yes |
| 200 | D6 | default | reduced, PlaneGCS System::solve + fallback | 300 | 0.132 (0.131 to 0.134) | 0.180 | 0.242 | 0 | 5.3e-15 | 0 | 0 | yes |
| 200 | D6 | default | reduced, sparse + fallback | 300 | 0.090 (0.089 to 0.091) | 0.112 | 0.547 | 0 | 5.3e-15 | 0 | 0 | yes |
| 200 | D6 | re-init | full, PlaneGCS System::solve (reference) | 300 | 390.467 (388.631 to 398.475) | 786.689 | 923.148 | 0 | 0 | 0 | 0 | yes |
| 200 | D6 | re-init | full, sparse | 300 | 3.464 (3.390 to 3.492) | 6.999 | 32.301 | 0 | 1.4e-14 | 0 | 0 | yes |
| 200 | D6 | re-init | reduced, PlaneGCS System::solve + fallback | 300 | 0.523 (0.520 to 0.534) | 0.679 | 2.549 | 0 | 2.1e-14 | 0 | 0 | yes |
| 200 | D6 | re-init | reduced, sparse + fallback | 300 | 0.089 (0.087 to 0.092) | 0.120 | 0.453 | 0 | 2.1e-14 | 0 | 0 | yes |
