## Changelog

### Unreleased

- **Breaking:** Drop support for Node.js versions lower than 20; the package now requires **Node.js 20 or later** (`engines.node: ">=20.0.0"`).
- **Breaking:** Upgrade native build tooling to **`cmake-js` ^8.0.0** and **`node-addon-api` ^8.0.0**, aligning with the Node.js 20+ support window.
- Clarify Node.js version requirement in the README.
- Add **Node.js API testing guide** at `tools/nodejs_api/docs/nodejs_testing.md` for test authors and reviewers (assertions, isolation, data types, concurrency, errors, resource lifecycle, validation checklist). Remove `tools/nodejs_api/test/test_correctness_audit.md` in favor of this guide.

