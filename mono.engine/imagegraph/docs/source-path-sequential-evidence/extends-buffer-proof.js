// Source-equation profile, not a licensed runner capture.
const assert = require('assert'), fs = require('fs');
const sourceChild={length:10, point(ratio,out){out.x=10*ratio;out.y=0;out.weight=3;return out;}};
const constructor={pathLength:10,segments:2,accumulated:[0,10],bbox:[0,0,10,0],endX:9.99};
const cache=new Map();
function distance(d,out){
 out.x=out.y=0;
 // These distinct distances test buffer reuse; key formatting is tested separately.
 const key=String(d);
 if(cache.has(key)) return Object.assign(out,cache.get(key));
 if(d>constructor.pathLength)out.x=constructor.endX+(d-constructor.pathLength);
 else sourceChild.point(d/constructor.pathLength,out);
 cache.set(key,{...out});return out;
}
const retained={x:0,y:0,weight:1};
const first={...distance(7.5,retained)};
const second={...distance(13.5,retained)};
const fresh={...distance(14,{x:0,y:0,weight:1})};
assert.equal(first.weight,3);assert.equal(second.weight,3);assert.equal(fresh.weight,1);
assert(Math.abs(second.x-13.49)<1e-12);
const proof={profile:'source-equations, fresh and reused caller out buffer',constructor,first,second,fresh};
fs.writeFileSync(__dirname+'/extends-buffer-vectors.json',JSON.stringify(proof,null,2)+'\n');console.log(JSON.stringify(proof));
