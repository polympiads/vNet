# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

import importlib.util
import os
import shutil
import subprocess

spec   = importlib.util.spec_from_file_location( "generate_gitdata", os.path.join(os.path.dirname( __file__ ), "generate_gitdata.py") )
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

project = 'vNet'
copyright = '2025, polympiads'
author = 'polympiads'
release = '1.0'

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = ['breathe']

# The C++ API reference: Doxygen writes XML from the headers, Breathe reads
# it. Doxygen runs on every build so the reference follows the code.
_here = os.path.dirname(os.path.abspath(__file__))
if shutil.which('doxygen'):
    subprocess.run(['doxygen', 'Doxyfile'], cwd=_here, check=True)
breathe_projects = {'vnet': os.path.join(_here, '_doxygen', 'xml')}
breathe_default_project = 'vnet'
breathe_domain_by_extension = {'h': 'cpp', 'hpp': 'cpp'}

templates_path = ['_templates']
exclude_patterns = ['_build', '_doxygen']

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'alabaster'
html_static_path = ['_static']
html_css_files = ['custom.css']
html_title = 'vNet documentation'
html_theme_options = {
    'github_user': 'polympiads',
    'github_repo': 'vNet',
    'github_button': False,
    'fixed_sidebar': True,
    'page_width': '1160px',
    'sidebar_width': '260px',
    # Plain system fonts: sans-serif text, monospace code.
    'font_family': 'system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif',
    'head_font_family': 'system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif',
    'code_font_family': 'ui-monospace, "SFMono-Regular", Menlo, Consolas, "DejaVu Sans Mono", monospace',
    'font_size': '15px',
    'caption_font_size': '15px',
    'code_font_size': '0.9em',
}

# Blocks name their language; plain :: blocks are shown as text.
highlight_language = "none"

html_version_root = "https://polympiads.github.io/vNet/"

html_sidebars = {
   '**': ['about.html', 'sidebar-search.html', 'sidebar-nav.html', 'versions.html']
}
html_context = module.generate_gitdata(html_version_root)
