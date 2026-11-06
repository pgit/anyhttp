# Documentation

The documentation has two parts, and is made into one site, published at
<https://pgit.github.io/anyhttp/>:

* the **guide**: the `.rst` files in here, written by hand.
  [`programmers-guide.rst`](programmers-guide.rst) is the place to start, and the others are about
  how it all works. Its code is in [`snippets.cpp`](snippets.cpp) and [`examples/`](examples), which
  are compiled with every build, and [`test/test_docs.cpp`](../test/test_docs.cpp) runs it.
* the **reference**: [`reference.rst`](reference.rst) says what goes where, and the content comes
  from the comments in the public headers in [`include/anyhttp`](../include/anyhttp).

The `.md` files in here are working notes, which the site links to rather than includes, see
[`notes.rst`](notes.rst).

## Making it

```sh
cmake --preset build
cmake --build build --target docs
```

The pages end up in `build/docs/html`, with `index.html` to start from. It takes
[Doxygen](https://www.doxygen.nl), [Sphinx](https://www.sphinx-doc.org),
[Breathe](https://breathe.readthedocs.io) and the
[Read the Docs theme](https://sphinx-rtd-theme.readthedocs.io), which the devcontainer has. On
Debian that is:

```sh
apt-get install doxygen python3-sphinx python3-breathe python3-sphinx-rtd-theme
```

The target is not part of a regular build, and not there at all where CMake finds no `doxygen` or no
`sphinx-build`, or was told `ANYHTTP_BUILD_DOCS=OFF`. It can be made from a tree of either I/O runtime
and gives the same result: Doxygen reads `asio/runtime.hpp` for the runtime layer, whichever runtime
the tree is on.

## Writing it

Warnings are errors in both tools, so a public symbol without a comment, a parameter without a
`\param` or a reference that goes nowhere fails the build, and with it a pull request.

* A new public symbol goes into [`reference.rst`](reference.rst), as nothing puts it there by itself.
  What a public header has for the library's own use goes between `\cond internal` and `\endcond`.
* In a comment, Doxygen takes `http::...` and `https://` for a link, and `\c` of either, or of a word
  that starts with `[`, comes out empty, which neither tool warns about. So write `errors::...`, as
  that is the spelling that holds on both I/O runtimes anyway, and `<tt>[...]</tt>`.
* The runtime layer has one section for both I/O runtimes. Its functions come from `asio/runtime.hpp`
  as everything else does, and so do the codes of `errors::`, each with `\hideinitializer` and a
  `\runtimes{asio, corosio}` that says what it is on each (an alias in `Doxyfile.in`). Its types,
  `Task`, `IoTask`, `results`, `error_code` and the others, are declared in `reference.rst` instead,
  as Breathe would show what they stand for on ASIO only; keep those in step with the headers.
* Code in the guide comes from `snippets.cpp`, between a `// [name]` and a `// [name-end]` comment,
  and is shown with `literalinclude`. Only pseudo code is written in place.
* In a page, names link to the reference with `` :cpp:func:`Session::submit` `` and the like, and
  files of the repository with `` :src:`src/server.cpp` ``.
* In prose, the libraries are Asio, Corosio and Capy. `ASIO` and `COROSIO` are the I/O runtimes, the
  values of `ANYHTTP_API`: "an ASIO build". Capy is named only where Capy itself is meant.

[`Doxyfile.in`](Doxyfile.in) configures Doxygen and [`conf.py`](conf.py) configures Sphinx.
