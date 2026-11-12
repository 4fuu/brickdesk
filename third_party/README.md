# Third-party code

Third-party sources are vendored here so host and target builds do not depend
on system packages beyond SDL2 and GLES2.

| Directory | Upstream | Revision | License |
|---|---|---|---|
| `stb/` | https://github.com/nothings/stb (`stb_image.h`, `stb_image_write.h`, `stb_truetype.h`) | `78770d85c4d94787a56c7859017da963bc4f3439` | MIT / public domain (dual) |
| `jsmn/` | https://github.com/zserge/jsmn (`jsmn.h`) | `25647e692c7906b96ffd2b05ca54c097948e879c` | MIT |
| `inih/` | https://github.com/benhoyt/inih (`ini.c`, `ini.h`) | `611ab51f8dd7ec583543815b0de693495045fbf8` | BSD-3-Clause |

The license text for each project is kept at the top of its header. The
`stb` headers carry the dual MIT/public-domain license text inline.
