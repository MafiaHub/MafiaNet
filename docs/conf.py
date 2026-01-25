# Configuration file for the Sphinx documentation builder.
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import subprocess
import os

# -- Project information -----------------------------------------------------

project = 'MafiaNet'
copyright = '2024, MafiaHub'
author = 'MafiaHub'
version = '0.2.0'
release = '0.2.0'

# -- General configuration ---------------------------------------------------

extensions = [
    'breathe',
    'sphinx.ext.autodoc',
    'sphinx.ext.viewcode',
    'sphinx.ext.todo',
    'sphinx.ext.graphviz',
    'sphinx_copybutton',
]

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store', '.venv', 'venv']

# -- Options for HTML output -------------------------------------------------

html_theme = 'furo'
html_static_path = ['_static']
html_title = 'MafiaNet Documentation'
html_logo = None
html_favicon = None

# Furo theme options
html_theme_options = {
    "light_css_variables": {
        "color-brand-primary": "#2962ff",
        "color-brand-content": "#2962ff",
    },
    "dark_css_variables": {
        "color-brand-primary": "#5c8aff",
        "color-brand-content": "#5c8aff",
    },
    "sidebar_hide_name": False,
    "navigation_with_keys": True,
    "top_of_page_button": "edit",
}

# -- Breathe configuration ---------------------------------------------------

breathe_projects = {
    'MafiaNet': '_build/doxygen/xml'
}
breathe_default_project = 'MafiaNet'
breathe_default_members = ('members', 'undoc-members')

# -- Extension configuration -------------------------------------------------

todo_include_todos = True

# -- Custom build step to run Doxygen ----------------------------------------

def run_doxygen(app):
    """Run doxygen before Sphinx build."""
    docs_dir = os.path.dirname(os.path.abspath(__file__))
    doxygen_output = os.path.join(docs_dir, '_build', 'doxygen', 'xml')

    if not os.path.exists(doxygen_output):
        print("Running Doxygen...")
        subprocess.call(['doxygen', 'Doxyfile'], cwd=docs_dir)

def setup(app):
    app.connect('builder-inited', run_doxygen)
