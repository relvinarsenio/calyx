# 🛠️ Contributing to Calyx

Thank you for your interest in contributing to **Calyx**! As a project focused on high-performance system benchmarking using modern standards like **C++23** and **io_uring**, your help is greatly appreciated.

### 📜 Contribution Policy (CLA)

To ensure the protection of your intellectual property and the long-term security of the project, contributors are expected to understand the following policies:

1.  **Small Code Exception**: You **do not need** a formal agreement for small contributions such as bug fixes, trivial patches, or documentation improvements.
2.  **Ownership**: You retain the copyright to your code. By submitting a contribution, you grant the **Calyx Project** permission (license) to use, distribute, and modify the code under the terms of the **Mozilla Public License 2.0 (MPL 2.0)**.
3.  **Third-Party Representations**: If your contribution involves third-party property (e.g., code owned by your employer), you represent that you have official permission or that they have waived such rights for the code to be released as open source.
4.  **Signed-off-by**: We strongly encourage the use of the **Developer Certificate of Origin (DCO)**. Simply add the `-s` flag when committing:
    ```bash
    git commit -s -m "feat: optimize io_uring batching"
    ```

5.  **Code of Conduct**: By contributing to this project, you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).

---

### 🚀 How to Contribute

1.  **Fork** this repository.
2.  Create a new **branch** for your feature or fix.
3.  Ensure your code passes automatic formatting using the **`.clang-format`** provided in the root directory.
4.  **Commit** your changes with clear messages (and the `-s` flag).
5.  Submit a **Pull Request** to the `experimental` branch (`master` is reserved for stable releases).
6.  All Pull Requests are integrated via **Squash and merge** to maintain a clean commit history.

### 🛡️ Coding Standards

*   **Modern C++**: Use **C++23** features where applicable.
*   **No Comments**: We prefer self-explanatory code. Do not insert comments unless absolutely critical for extremely complex logic.
*   **Safety First**: Always prioritize memory safety and Resource Management (RAII).
*   **Structure**: Follow the existing modular organization in `src/core`, `src/io`, `src/net`, etc.

---

If you have further questions regarding legal or technical aspects, feel free to reach out at alfieardinata@outlook.com.