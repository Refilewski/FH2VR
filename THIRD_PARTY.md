# Third-party components

## FH2ML SDK header

`include/FH2ML.h` is copied from the FH2ML SDK header and is distributed upstream under the MIT license for the SDK headers.

## OpenVR

FH2VR builds against Valve's OpenVR SDK v2.15.6. CMake fetches the SDK source at configure time; OpenVR source is not committed to this repository.

The packaged FH2VR artifact includes the complete upstream OpenVR license at:

```text
licenses/OpenVR-LICENSE.txt
```

OpenVR is distributed by Valve under the BSD 3-Clause license.

## MinHook

FH2VR builds against MinHook v1.3.4 for the renderer hooks. CMake fetches MinHook at configure time; MinHook source is not committed to this repository.

The packaged FH2VR artifact includes the complete upstream MinHook/HDE notices and license text at:

```text
licenses/MinHook-LICENSE.txt
```

See that packaged upstream file for the complete redistribution conditions and notices.
