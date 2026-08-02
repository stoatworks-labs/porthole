#pragma once

/**
    The one shader pass.

    Porthole is a resampler: every output pixel works out which direction it is
    looking, asks the lens where that ray came from in the source picture, and
    fetches it. There is nothing to accumulate between frames and nothing that
    depends on any other pixel, so unlike the rest of the fleet's shader work
    this is a single pass with no intermediate buffers at all.

    Four things in it are worth knowing about before editing:

    - **The projection maths is a mirror of Projection.cpp.** Two copies of one
      formula is a liability, and the answer to it is `phtest --probe`, which
      measures what the GPU actually did against what the C++ predicts.

    - **Everything happens in picture space, 0..1, and MaxUV is applied at the
      last possible moment.** FFGL can hand over a texture larger than the
      picture in it. A warp samples wherever it likes, so treating the texture
      edge as the picture edge fetches undrawn padding.

    - **The supersample grid is spread using dFdx/dFdy of the output UV**, not a
      resolution uniform, so it is right whatever the host renders at.

    - **Uniform names have to match the C++ exactly.** A mismatch is not an
      error anywhere: glGetUniformLocation returns -1 and glUniform on -1 is a
      documented no-op, so the control is simply dead. `tools/sweep.py` is the
      only thing that catches it.
*/
namespace porthole
{

/// Passes UV through untouched, in 0..1 picture space. Deliberately *not*
/// pre-multiplied by MaxUV the way a simple filter's vertex shader would --
/// the fragment shader needs to do its geometry in picture space and scale
/// only the final fetch.
extern const char* const kVertexShader;

extern const char* const kFragmentShader;

} // namespace porthole
