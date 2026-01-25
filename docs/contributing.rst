Contributing
============

We welcome contributions to MafiaNet! This guide will help you get started.

Getting Started
---------------

1. Fork the repository on GitHub
2. Clone your fork locally
3. Create a branch for your changes

.. code-block:: bash

   git clone https://github.com/YOUR_USERNAME/MafiaNet.git
   cd MafiaNet
   git checkout -b feature/my-new-feature

Code Style
----------

* Use C++17 features where appropriate
* Follow existing code formatting conventions
* Add comments for complex logic
* Keep functions focused and reasonably sized

Building and Testing
--------------------

Build with tests enabled:

.. code-block:: bash

   mkdir build && cd build
   cmake -DMAFIANET_BUILD_TESTS=ON ..
   cmake --build .
   ./Samples/Tests/Tests

Submitting Changes
------------------

1. Ensure all tests pass
2. Commit your changes with a clear message
3. Push to your fork
4. Open a Pull Request

License
-------

By contributing, you agree that your contributions will be licensed under the MIT License.
