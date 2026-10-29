# Third-Party Licenses and Acknowledgements

Pixel VR Player makes use of several open-source software libraries, components, and standards. We are grateful to the authors and open-source communities for their contributions.

---

## 1. OpenXR-SDK
- **Copyright:** (c) 2017-2024 The Khronos Group Inc.
- **License:** Apache License, Version 2.0
- **Website:** https://github.com/KhronosGroup/OpenXR-SDK

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at:
http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific language governing permissions and limitations under the License.

---

## 2. cgltf
- **Copyright:** (c) 2018-2024 Johannes Kuhlmann
- **License:** MIT License
- **Website:** https://github.com/jkuhlmann/cgltf

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

## 3. stb_image
- **Author:** Sean Barrett and contributors
- **License:** MIT License / Public Domain (Unlicense)
- **Website:** https://github.com/nothings/stb

This software is dual-licensed to the public domain and under the following license: you are free to use this software under the terms of the MIT License or the Unlicense.

---

## 4. OpenVR
- **Copyright:** (c) Valve Corporation
- **License:** BSD 3-Clause License
- **Website:** https://github.com/ValveSoftware/openvr

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## 5. FreeType
- **Copyright:** (c) 1996-2024 David Turner, Robert Wilhelm, and Werner Lemberg
- **License:** The FreeType Project License (FTL)
- **Website:** https://www.freetype.org

Portions of this software are copyright (c) 2024 The FreeType Project (www.freetype.org). All rights reserved.

---

## 6. FFmpeg
- **Copyright:** (c) 2000-2024 the FFmpeg developers
- **License:** GNU Lesser General Public License (LGPL), version 2.1 or later
- **Website:** https://ffmpeg.org

Pixel VR Player uses dynamically linked FFmpeg libraries (`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`). FFmpeg is licensed under the LGPL v2.1+. Pixel VR Player does not incorporate any GPL-only or non-free FFmpeg modules.

In accordance with the LGPL:
- FFmpeg is linked dynamically.
- The source code for FFmpeg and our specific V4L2 zero-copy DMA-BUF patch set is provided in the repository under `third_party/ffmpeg/`.
- Full FFmpeg source code is available at https://ffmpeg.org.

---

## 7. PulseAudio Client Library
- **License:** GNU Lesser General Public License (LGPL), version 2.1 or later
- **Website:** https://www.freedesktop.org/wiki/Software/PulseAudio/

Dynamically linked for audio output on Linux systems.

---

## 8. Samba Client Library (libsmbclient)
- **License:** GNU Lesser General Public License (LGPL), version 3 or later
- **Website:** https://www.samba.org

Dynamically linked for SMB network share streaming and discovery.

---

## 9. Vulkan
- **Copyright:** (c) 2015-2024 The Khronos Group Inc.
- **License:** Apache License, Version 2.0
- **Website:** https://www.khronos.org/vulkan/

Vulkan and the Vulkan logo are trademarks of the Khronos Group Inc.

---

## 10. Fonts
System UI text is rendered using fonts available on the host operating system:
- **Noto Sans:** Licensed under the SIL Open Font License, Version 1.1 (OFL).
- **DejaVu Sans:** Licensed under the Bitstream Vera Fonts License and Free Software Foundation public domain dedication.
