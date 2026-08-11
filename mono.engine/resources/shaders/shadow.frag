#version 450

// Writes nothing. The depth attachment is the output.
//
// **A fragment shader is still required**, even for a depth-only pass: SDL's
// GPU API takes both stages when it creates a graphics pipeline, and a null
// fragment stage is not one of the things it accepts. An empty `main` is what
// every backend compiles this to anyway.

void main() {
}
