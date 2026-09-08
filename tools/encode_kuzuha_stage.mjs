// Encode already-rendered production frames; this does not launch the demo.
import {spawn} from 'node:child_process';
import {stat} from 'node:fs/promises';
import {resolve,join} from 'node:path';
const [ffmpegInput,directoryInput,outputInput]=process.argv.slice(2);
if(!ffmpegInput||!directoryInput||!outputInput)
    throw new Error('Usage: encode_kuzuha_stage.mjs <ffmpeg.exe> <frame-directory> <output.mp4>');
const ffmpeg=resolve(ffmpegInput),directory=resolve(directoryInput),output=resolve(outputInput);
await stat(ffmpeg);
for(let i=0;i<432;++i) await stat(join(directory,`frame-${String(i).padStart(5,'0')}.bmp`));
const labels="drawtext=fontfile='C\\:/Windows/Fonts/arial.ttf':text='KUZUHA':x=28:y=28:fontsize=24:fontcolor=white,drawtext=fontfile='C\\:/Windows/Fonts/arial.ttf':text='STAGE C  /  PN RAYMARCH':x=28:y=60:fontsize=13:fontcolor=0x9999aa,fade=t=in:st=0:d=0.3,fade=t=out:st=17.5:d=0.5";
const encoder=spawn(ffmpeg,['-y','-framerate','24','-i',join(directory,'frame-%05d.bmp'),
    '-frames:v','432','-vf',labels,'-c:v','libx264','-preset','slow','-crf','17',
    '-pix_fmt','yuv420p','-movflags','+faststart',output],{windowsHide:true,stdio:'inherit'});
await new Promise((accept,reject)=>{
    encoder.once('error',reject);
    encoder.once('exit',(code,signal)=>code===0?accept():reject(new Error(`Encoder failed: ${code??signal}`)));
});
