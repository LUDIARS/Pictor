import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// Offline SDK tool build; this repository owns every APK source and asset.
const source = dirname(fileURLToPath(import.meta.url));
const root = resolve(source, '../..');
const sdk = process.env.ANDROID_HOME;
const ndk = process.env.ANDROID_NDK_HOME;
const javaHome = process.env.JAVA_HOME;
if (!sdk || !ndk || !javaHome) throw new Error('Set ANDROID_HOME, ANDROID_NDK_HOME and JAVA_HOME explicitly');
const suffix = process.platform === 'win32' ? '.exe' : '';
const java = join(javaHome, 'bin', `java${suffix}`);
const toolVersion = process.env.ANDROID_BUILD_TOOLS ?? '34.0.0';
const tools = join(sdk, 'build-tools', toolVersion);
const platform = join(sdk, 'platforms', process.env.ANDROID_COMPILE_PLATFORM ?? 'android-31', 'android.jar');
const out = join(root, 'build', 'android-demo');
const stage = join(out, 'package');
for (const dir of [out, stage, join(stage, 'assets'), join(stage, 'lib', 'arm64-v8a'), join(out, 'classes')]) mkdirSync(dir, { recursive: true });
function run(executable, args, cwd = root) {
  const result = spawnSync(executable, args, { cwd, stdio: 'inherit', shell: false, windowsHide: true });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${executable} exited ${result.status}`);
}
for (const path of [java, platform, join(tools, `aapt2${suffix}`), join(ndk, 'build', 'cmake', 'android.toolchain.cmake')]) {
  if (!existsSync(path)) throw new Error(`Required build tool is missing: ${path}`);
}
const native = join(out, 'native');
const configure = ['-S', source, '-B', native, '-G', 'Ninja',
  `-DCMAKE_TOOLCHAIN_FILE=${join(ndk, 'build', 'cmake', 'android.toolchain.cmake')}`,
  '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Debug',
  `-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${join(stage, 'lib', 'arm64-v8a')}`];
if (process.env.NINJA) configure.push(`-DCMAKE_MAKE_PROGRAM=${process.env.NINJA}`);
run('cmake', configure);
run('cmake', ['--build', native, '--target', 'pictor_demos', '--parallel', '4']);
const hostTag = process.platform === 'win32' ? 'windows-x86_64' : process.platform === 'darwin' ? 'darwin-x86_64' : 'linux-x86_64';
const glslc = join(ndk, 'shader-tools', hostTag, `glslc${suffix}`);
for (const shader of ['simple_inst.vert', 'simple_inst.frag']) {
  run(glslc, [join(root, 'demo', 'shaders', shader), '-o', join(stage, 'assets', `${shader}.spv`)]);
}
const javaSources = join(source, 'java', 'com', 'ludiars', 'pictor', 'demos');
run(join(javaHome, 'bin', `javac${suffix}`), ['-encoding', 'UTF-8', '-source', '8', '-target', '8', '-classpath', platform,
  '-d', join(out, 'classes'), ...readdirSync(javaSources).filter(n => n.endsWith('.java')).map(n => join(javaSources, n))]);
function classFiles(path) {
  return readdirSync(path, { withFileTypes: true }).flatMap(e => e.isDirectory() ? classFiles(join(path, e.name)) : e.name.endsWith('.class') ? [join(path, e.name)] : []);
}
run(java, ['-cp', join(tools, 'lib', 'd8.jar'), 'com.android.tools.r8.D8', '--lib', platform, '--min-api', '28', '--output', stage, ...classFiles(join(out, 'classes'))]);
const unsigned = join(out, 'unsigned.apk');
run(join(tools, `aapt2${suffix}`), ['link', '-o', unsigned, '-I', platform, '--manifest', join(source, 'AndroidManifest.xml')]);
run(join(javaHome, 'bin', `jar${suffix}`), ['uf', unsigned, '-C', stage, '.']);
const aligned = join(out, 'aligned.apk');
run(join(tools, `zipalign${suffix}`), ['-f', '4', unsigned, aligned]);
const key = join(out, 'debug.keystore');
if (!existsSync(key)) run(join(javaHome, 'bin', `keytool${suffix}`), ['-genkeypair', '-keystore', key,
  '-storepass', 'android', '-keypass', 'android', '-alias', 'androiddebugkey', '-keyalg', 'RSA', '-keysize', '2048',
  '-validity', '10000', '-dname', 'CN=Android Debug,O=Pictor,C=JP']);
const apk = join(out, 'pictor-demos.apk');
run(java, ['-jar', join(tools, 'lib', 'apksigner.jar'), 'sign', '--ks', key, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, aligned]);
run(java, ['-jar', join(tools, 'lib', 'apksigner.jar'), 'verify', apk]);
console.log(`APK: ${apk}`);
