# Premiere Pro sample projects

Real `.prproj` files saved by Premiere Pro 26.3, each with the ExtendScript dump of what Premiere itself
reports for it (`.json`: sequences, tracks, clip start/end/in/out, markers). They come unmodified from
[py-premiere](https://github.com/Pouf/py-premiere) (`samples/models/minimal`), MIT licensed — see
`LICENSE-py-premiere.txt`. `test_prproj.cpp` checks Wabcut's experimental reader against the dumps.
