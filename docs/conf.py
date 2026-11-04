#
# Sphinx makes the HTML, of the guide as well as of the reference, whose content Breathe takes from
# the XML that Doxygen makes of the public headers. 'cmake --build build --target docs' runs both
# and says where the XML is and which version this is, see CMakeLists.txt in here.
#
from docutils import nodes

project = "anyhttp"
author = "Peter Eisenlohr"
copyright = "2026, Peter Eisenlohr"

extensions = ["breathe", "sphinx.ext.extlinks"]

# a file of the repository, which the pages link to rather than copy
extlinks = {"src": ("https://github.com/pgit/anyhttp/blob/master/%s", "%s")}

breathe_default_project = "anyhttp"
breathe_default_members = ()

primary_domain = "cpp"
highlight_language = "cpp"
cpp_index_common_prefix = ["anyhttp::"]

html_theme = "sphinx_rtd_theme"
html_theme_options = {"navigation_depth": 3}

# the Markdown files in here are working notes, which the pages link to on GitHub
exclude_patterns = ["*.md", "_build"]

#
# A reference that goes nowhere is an error, but only for what is there to go to: the names of the
# standard library, Boost and the runtimes are not documented here, and a namespace is only ever
# part of the name of what is in it.
#
nitpicky = True
nitpick_ignore_regex = [
    (r"cpp:.*", r"(std|boost|asio|capy|corosio)(::.*)?"),
    (r"cpp:.*", r"(u?int\d+_t|size_t)"),
    (r"cpp:.*", r"(errc(::.*)?|make_error_code)"),
    (r"cpp:identifier", r"anyhttp(::(client|server|quic|errors))?"),
    (r"cpp:identifier", r"(client|server|quic|errors)"),
]


def fix_dashes(app, doctree):
    """Breathe 4.35 turns a "--" in a comment into the text "&#8212;", which then shows as just that.

    What it stands for is lost by then, but Doxygen would only make an em dash of "---", which the
    comments do not use.
    """
    for node in list(doctree.findall(nodes.Text)):
        if "&#8212;" in node:
            node.parent.replace(node, nodes.Text(node.replace("&#8212;", "\N{EN DASH}")))


def setup(app):
    app.connect("doctree-read", fix_dashes)
