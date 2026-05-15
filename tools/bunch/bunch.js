#!/usr/bin/env node
// Positron Bunch - module bundler
//
// Usage:
//   node bunch.js build [dir]      Build the bunch project in [dir] (default: cwd)
//   node bunch.js build .          Build current directory
//
// Reads bunch.json, compiles .ts files with tsc, packs everything into a
// VFS-backed positron module. Output: .build/<name>.b.js (minified)

const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

// -------------------------------------------------------------------------
// Config
// -------------------------------------------------------------------------

const IGNORE_PATTERNS = [
  /^\.build\//,
  /^node_modules\//,
  /^\.git\//,
  /\.d\.ts$/,
  /^bunch\.json$/,
  /^tsconfig\.json$/,
];

function shouldIgnore(relPath) {
  for (const pat of IGNORE_PATTERNS) {
    if (pat.test(relPath)) return true;
  }
  return false;
}

// -------------------------------------------------------------------------
// Collect files recursively
// -------------------------------------------------------------------------

function collectFiles(dir, base) {
  base = base || dir;
  let result = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    const rel = path.relative(base, full).replace(/\\/g, '/');
    if (shouldIgnore(rel)) continue;
    if (entry.isDirectory()) {
      result = result.concat(collectFiles(full, base));
    } else {
      result.push({ rel, full });
    }
  }
  return result;
}

// -------------------------------------------------------------------------
// Escape string for JS string literal
// -------------------------------------------------------------------------

function escapeForJS(str) {
  // Escape for embedding in a JS double-quoted string literal.
  // Process char-by-char to avoid regex interaction issues.
  var out = '';
  for (var i = 0; i < str.length; i++) {
    var c = str[i];
    switch (c) {
      case '\\': out += '\\\\'; break;
      case '"':  out += '\\"';  break;
      case '\n': out += '\\n';  break;
      case '\r': out += '\\r';  break;
      case '\t': out += '\\t';  break;
      case '\0': out += '\\0';  break;
      default:   out += c;      break;
    }
  }
  return out;
}

// -------------------------------------------------------------------------
// Main build
// -------------------------------------------------------------------------

function build(projectDir) {
  projectDir = path.resolve(projectDir || '.');
  const bunchJsonPath = path.join(projectDir, 'bunch.json');

  if (!fs.existsSync(bunchJsonPath)) {
    console.error('Error: bunch.json not found in ' + projectDir);
    process.exit(1);
  }

  const config = JSON.parse(fs.readFileSync(bunchJsonPath, 'utf8'));
  const name = config.name || 'unnamed';
  const entry = (config.entry || 'main.js').replace(/\.(js|ts)$/, '');
  const version = config.version || '0.0.0';

  console.log(`[bunch] Building "${name}" v${version} from ${projectDir}`);

  const buildDir = path.join(projectDir, '.build');
  const objDir = path.join(buildDir, 'obj');
  fs.mkdirSync(objDir, { recursive: true });

  // 1. Collect all source files
  const files = collectFiles(projectDir);
  const tsFiles = files.filter(f => f.rel.endsWith('.ts'));
  const otherFiles = files.filter(f => !f.rel.endsWith('.ts'));

  console.log(`[bunch] Found ${tsFiles.length} .ts files, ${otherFiles.length} other files`);

  // 2. Compile TypeScript files
  if (tsFiles.length > 0) {
    // Write a minimal tsconfig for compilation
    const tsconfig = {
      compilerOptions: {
        target: 'ES2015',
        module: 'none',
        outDir: objDir,
        rootDir: projectDir,
        strict: false,
        skipLibCheck: true,
        esModuleInterop: true,
        removeComments: true,
        declaration: false,
      },
      include: tsFiles.map(f => f.full),
    };
    const tsconfigPath = path.join(buildDir, 'tsconfig.bunch.json');
    fs.writeFileSync(tsconfigPath, JSON.stringify(tsconfig, null, 2));

    console.log('[bunch] Compiling TypeScript...');
    try {
      execSync(`tsc -p "${tsconfigPath}"`, { cwd: projectDir, stdio: 'pipe' });
    } catch (e) {
      // tsc may emit warnings but still produce output
      const stderr = e.stderr ? e.stderr.toString() : '';
      if (stderr.includes('error TS')) {
        console.error('[bunch] TypeScript errors:\n' + stderr);
        process.exit(1);
      }
      if (stderr) console.warn('[bunch] TypeScript warnings:\n' + stderr);
    }
  }

  // 3. Build VFS entries
  const vfsEntries = [];

  // Add compiled .ts -> .js files
  for (const f of tsFiles) {
    const jsRel = f.rel.replace(/\.ts$/, '.js');
    const jsPath = path.join(objDir, jsRel);
    if (fs.existsSync(jsPath)) {
      const content = fs.readFileSync(jsPath, 'utf8');
      const vfsKey = f.rel.replace(/\.ts$/, '');  // strip extension for VFS key
      vfsEntries.push({ key: vfsKey, content });
    } else {
      console.warn(`[bunch] Warning: compiled output not found for ${f.rel}`);
    }
  }

  // Add non-ts files as-is
  for (const f of otherFiles) {
    const content = fs.readFileSync(f.full, 'utf8');
    vfsEntries.push({ key: f.rel, content });
  }

  console.log(`[bunch] Packing ${vfsEntries.length} files into VFS`);

  // 4. Load runtime template
  const runtimePath = path.join(__dirname, 'runtime.js');
  let runtime = fs.readFileSync(runtimePath, 'utf8');

  // 5. Generate VFS write statements (base64 encoded to avoid escape issues)
  const vfsStatements = vfsEntries.map(e => {
    const b64 = Buffer.from(e.content, 'utf8').toString('base64');
    return `_vfs.write("${escapeForJS(e.key)}", _b64("${b64}"));`;
  }).join('\n  ');

  // 6. Substitute template variables
  runtime = runtime.replace(/__BUNCH_NAME__/g, escapeForJS(name));
  runtime = runtime.replace(/__BUNCH_ENTRY__/g, escapeForJS(entry));
  runtime = runtime.replace(/\/\/ __BUNCH_VFS_ENTRIES__/, vfsStatements);

  // 7. Write unminified to obj
  const rawPath = path.join(objDir, name + '.b.raw.js');
  fs.writeFileSync(rawPath, runtime);
  console.log(`[bunch] Raw bundle: ${rawPath} (${(runtime.length / 1024).toFixed(1)} KB)`);

  // 8. Minify with terser
  const outPath = path.join(buildDir, name + '.b.js');
  console.log('[bunch] Minifying...');
  try {
    // Find terser: try project root node_modules first, then global
    const positronRoot = path.resolve(__dirname, '../..');
    const terserLocal = path.join(positronRoot, 'node_modules', '.bin', 'terser');
    const terserCmd = fs.existsSync(terserLocal + '.cmd') || fs.existsSync(terserLocal)
      ? `"${terserLocal}"`
      : 'npx terser';

    execSync(`${terserCmd} "${rawPath}" --compress negate_iife=false,side_effects=false --mangle --output "${outPath}"`, {
      cwd: positronRoot,
      stdio: 'pipe'
    });
    const minSize = fs.statSync(outPath).size;
    console.log(`[bunch] Output: ${outPath} (${(minSize / 1024).toFixed(1)} KB)`);
  } catch (e) {
    console.warn('[bunch] Minification failed, using raw bundle');
    fs.copyFileSync(rawPath, outPath);
  }

  console.log(`[bunch] Done! Load with: .mod load ${path.relative(process.cwd(), outPath)}`);
}

// -------------------------------------------------------------------------
// Create
// -------------------------------------------------------------------------

function create(name) {
  if (!name) {
    console.error('Usage: bunch create <name>');
    process.exit(1);
  }

  const dir = path.resolve(name);
  if (fs.existsSync(dir)) {
    console.error(`Error: directory "${name}" already exists`);
    process.exit(1);
  }

  fs.mkdirSync(dir, { recursive: true });

  // bunch.json
  const config = { name: name, version: '0.0.1', entry: 'main.js' };
  fs.writeFileSync(path.join(dir, 'bunch.json'), JSON.stringify(config, null, 4) + '\n');

  // global.d.ts
  const dtsSource = path.join(__dirname, 'global.d.ts');
  fs.copyFileSync(dtsSource, path.join(dir, 'global.d.ts'));

  // main.ts
  const mainTs = `const BunchModule: PositronBunchModule = {
  name: '${name}',

  onLoad: function(api: PositronBunchApi) {
    api.log('${name} loaded');
  },

  onUnload: function(api: PositronBunchApi) {
    api.log('${name} unloaded');
  }
};
`;
  fs.writeFileSync(path.join(dir, 'main.ts'), mainTs);

  // tsconfig.json (for VS Code intellisense)
  const tsconfig = {
    compilerOptions: {
      target: 'ES2015',
      module: 'none',
      strict: false,
      skipLibCheck: true,
      noEmit: true
    },
    include: ['*.ts', '**/*.ts']
  };
  fs.writeFileSync(path.join(dir, 'tsconfig.json'), JSON.stringify(tsconfig, null, 4) + '\n');

  // data directory
  fs.mkdirSync(path.join(dir, 'data'));

  console.log(`[bunch] Created project "${name}" at ${dir}`);
  console.log('');
  console.log('  Files:');
  console.log('    bunch.json      project config');
  console.log('    global.d.ts     type definitions (editor autocompletion)');
  console.log('    main.ts         entry point');
  console.log('    data/           static resources (HTML, CSS, etc.)');
  console.log('');
  console.log('  Build:');
  console.log(`    node tools/bunch/bunch.js build ${name}`);
  console.log('');
  console.log('  Load:');
  console.log(`    .mod load ${name}/.build/${name}.b.js`);
}

// -------------------------------------------------------------------------
// CLI
// -------------------------------------------------------------------------

const args = process.argv.slice(2);
const command = args[0];

if (command === 'build') {
  build(args[1]);
} else if (command === 'create') {
  create(args[1]);
} else {
  console.log('Positron Bunch - module bundler');
  console.log('');
  console.log('Usage:');
  console.log('  bunch create <name>  Create a new bunch project');
  console.log('  bunch build [dir]    Build the bunch project');
  console.log('');
  console.log('The project directory must contain a bunch.json with:');
  console.log('  { "name": "my-module", "version": "0.0.1", "entry": "main.js" }');
}
