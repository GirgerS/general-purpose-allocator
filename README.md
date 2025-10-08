# Allocators
> [!WARNING]
> Right now this repo is merely a proof of concept, nothing more. All the features are yet to come.

Stb-style header-only library providing useful allocators:
- general-purpose allocator built on top of red-black tree
- static arena, aka scratch buffer, etc.

It in terms of speed GPA should be comparable to the standard allocator, but memory footprint of each allocation is almost two times larger

Features in progress:
- allocation alignment
- tools for memory profiling
- built-in gpa integrity validation
- pool and debug allocators
- 
For a quick start, see 'examples'
