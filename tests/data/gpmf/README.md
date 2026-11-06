# GoPro GPMF sample payloads

`hero5.raw` and `hero6.raw` are single raw GPMF payloads (one second of GoPro telemetry each) from the `samples` folder of
GoPro's GPMF parser, <https://github.com/gopro/gpmf-parser> (a GoPro HERO5 Black on firmware 2.00, a HERO6 Black on 1.60).
They are test data only (tests/ai/stabilize/test_gyro.cpp, tests/media/test_data_streams.cpp): Wabcut does not ship them.

| File | SHA-256 |
| --- | --- |
| `hero5.raw` | `53ce381152e6d90357d221a34220570ac1be380bcfdd88329126609d4582ee78` |
| `hero6.raw` | `be82cf6bf4fee3ed5b13b7d5683a62f03f2418403b4402dcaffec8e37aa1e2cf` |

The repository is dual licensed (Apache-2.0 or MIT, at the user's option); the MIT licence applies here:

> Copyright 2017 GoPro, Inc.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
> files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
> modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
> is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
> WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
> COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
> ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
