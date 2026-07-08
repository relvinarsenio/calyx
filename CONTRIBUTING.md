# 🛠️ Contributing to Calyx

Thank you for your interest in contributing to **Calyx**! As a project focused on high-performance system benchmarking using modern standards like **C++23** and **io_uring**, your help is greatly appreciated.

### 📜 Contribution Policy

To ensure the protection of your intellectual property and the long-term security of the project, contributors are expected to understand the following policies:

1.  **Developer Certificate of Origin (DCO)**: To ensure that this project has the right to distribute all contributions, we require that all contributors agree to the [Developer Certificate of Origin (DCO)](https://developercertificate.org/). This is a lightweight mechanism to affirm that you wrote the code and have the legal right to submit it.
    
    *   **How to Sign-off**: Add a `Signed-off-by` line to your commit messages using the `-s` flag. Pseudonyms are permitted as long as you provide a valid email address.
        ```bash
        git commit -s -m "feat: optimize io_uring batching"
        ```
    *   **Forgot to Sign-off?**: If you made a commit without the sign-off, you can easily add it by amending your commit:
        ```bash
        git commit --amend --signoff
        ```

2.  **Ownership**: You retain the copyright to your code. By submitting and signing off your contribution, you grant the **Calyx Project** permission to use, distribute, and modify the code under the terms of the **Mozilla Public License 2.0 (MPL 2.0)**.
3.  **Third-Party Representations**: If your contribution involves third-party property (e.g., code owned by your employer), your DCO sign-off represents that you have official permission or that they have waived such rights.
4.  **Code of Conduct**: By contributing to this project, you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).

---

### 🎯 Getting Started

Looking for a place to start?
*   **Find an Issue**: Check our issue tracker and look for labels like `good first issue` or `help wanted`.
*   **Communicate**: If you have questions before starting or need architectural guidance, feel free to open a GitHub Discussion or reach out to the maintainers.

### 🚀 How to Contribute

1.  **Fork & Branch**: Fork this repository and create a new branch for your feature or fix.
2.  **Build & Test**: Please refer to the [README.md](README.md) for detailed instructions on how to compile the project (via CMake) and run tests locally. Ensure all tests pass before submitting!
3.  **Format Code**: Ensure your code passes automatic formatting using the **`.clang-format`** provided in the root directory.
4.  **Commit Changes**: We follow the [Conventional Commits](https://www.conventionalcommits.org/) specification (`feat:`, `fix:`, `docs:`, `chore:`, etc.). Remember to include your DCO sign-off (`-s` flag) in every commit as required by our Contribution Policy.
5.  **Submit a Pull Request**: Submit your PR targeting the `development` branch (`master` is reserved for stable releases).

### 🛡️ Coding Standards

*   **No Legacy C or Old C++ Standards**: Strictly prohibit the use of old C-style paradigms (e.g., raw pointers, manual memory management, C-style arrays) and outdated C++ standards.
*   **Modern C++ Standards**: We embrace modern C++ paradigms. Please aim to utilize recent C++ standards (C++20, C++23, or newer) where appropriate to keep the codebase clean, safe, and future-proof.
*   **Fixed-Width Integers**: Use fixed-width integer types (e.g., `std::int32_t`, `std::uint32_t`, `std::size_t`) instead of bare types (like `int` or `long`). Exceptions are only permitted to satisfy external C/kernel API contracts.
*   **Meaningful & Concise Naming**: All identifiers must be meaningful, clear, and strictly concise without redundancy.
*   **Documentation & Rationale**: 
    *   Write self-documenting code. Never write comments explaining *how* the code works.
    *   Explain the *why* (rationale) and *what* (high-level intent) behind complex decisions or non-obvious logic.
    *   Use Doxygen-style documentation (`/** ... */`) for all public APIs and core infrastructure.
*   **Safety First**: Always prioritize memory safety and Resource Management (RAII).
*   **Structure**: Follow the existing modular organization in `src/core`, `src/io`, `src/net`, etc.

### 🏗️ Architecture & Core Guidelines

We believe in keeping things simple, robust, and safe. To achieve this, we adhere to foundational philosophies:
*   **[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)**: For memory safety, concurrency, and utilizing modern C++23 effectively.
*   **Unix Philosophy (KISS)**: Design for simplicity, modularity, and clarity. Avoid over-engineering and prioritize code that is easy to read, debug, and extend.
*   **SOLID Principles**: Ensure code is maintainable, scalable, and loosely coupled by adhering to Single Responsibility, Open/Closed, Liskov Substitution, Interface Segregation, and Dependency Inversion.

---

If you have further questions regarding legal or technical aspects, feel free to reach out at alfieardinata@outlook.com.