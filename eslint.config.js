const js = require("@eslint/js");
const n = require("eslint-plugin-n");
const globals = require("globals");
const prettier = require("eslint-config-prettier");

module.exports = [
  { ignores: ["node_modules/**", "build/**", "**/fixtures/**", "chrome_headers/**"] },
  js.configs.recommended,
  n.configs["flat/recommended-script"],
  {
    settings: {
      n: { version: ">=24.0.0" },
    },
    languageOptions: {
      ecmaVersion: "latest",
      sourceType: "commonjs",
      globals: {
        ...globals.node,
      },
    },
    rules: {
      "no-unused-vars": ["error", { varsIgnorePattern: "^_", argsIgnorePattern: "^_" }],
      // A catch that deliberately swallows is idiomatic here -- `catch {}` with
      // no binding already says the error is unwanted.
      "no-empty": ["error", { allowEmptyCatch: true }],
    },
  },
  {
    // .mjs is ESM regardless of the package's own type.
    files: ["**/*.mjs"],
    languageOptions: { sourceType: "module" },
  },
  {
    // The compiled addon under build/Release is produced by node-gyp, and by
    // electron-rebuild when the editor installs this. It is deliberately not in
    // `files`, so a lint run over the checkout cannot resolve it.
    files: ["index.js", "lib/**", "src/**/*.js"],
    rules: {
      "n/no-missing-require": "off",
      "n/no-unpublished-require": "off",
    },
  },
  {
    // The lint configuration itself requires devDependencies; it never ships.
    files: ["eslint.config.js", "prettier.config.js"],
    rules: {
      "n/no-unpublished-require": "off",
      "n/no-extraneous-require": "off",
    },
  },
  {
    files: ["spec/**", "test/**", "scripts/**", "benchmark/**"],
    languageOptions: {
      globals: {
        ...globals.jasmine,
        ...globals.mocha,
      },
    },
    rules: {
      "n/no-unpublished-require": "off",
      "n/no-extraneous-require": "off",
      "n/no-process-exit": "off",
    },
  },
  // Must be last: turns off any lint rules that would conflict with Prettier.
  prettier,
];
