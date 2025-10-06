# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

import importlib.util
import os

spec   = importlib.util.spec_from_file_location( "generate_gitdata", os.path.join(os.path.dirname( __file__ ), "generate_gitdata.py") )
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

project = 'vNet'
copyright = '2025, polympiads'
author = 'polympiads'
release = '1.0'

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = []

templates_path = ['_templates']
exclude_patterns = []

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'alabaster'
html_static_path = ['_static']

highlight_language = "python"

html_version_root = "https://polympiads.github.io/vNet/"

html_sidebars = {
   '**': ['about.html', 'searchbox.html', 'navigation.html', 'versions.html']
}
html_context = module.generate_gitdata(html_version_root)
