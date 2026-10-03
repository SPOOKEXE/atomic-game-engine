// Source-equation object profile copied from the pinned buffer admission branches.
const fs=require('fs'),assert=require('assert');
class P2 {constructor(x=0,y=0,w=1){this.x=x;this.y=y;this.weight=w;}toArray(){return [this.x,this.y];}}
class P3 {constructor(x=0,y=0,z=0,w=1){this.x=x;this.y=y;this.z=z;this.weight=w;}toArray(){return [this.x,this.y,this.z];}}
function raw3(r,out){if(!(out instanceof P3))out=new P3();else out.x=out.y=out.z=0;out.x=10*r;out.z=3;return out;}
function extendsInside(r,out){if(out===undefined)out=new P2();else out.x=out.y=0;raw3(r,out);return out;}
function flattenInside(r,out){if(out===undefined)out=new P2();else out.x=out.y=0;raw3(r,out);return out;}
function smoothPoint(r,out){if(out===undefined)out=new P2();else out.x=out.y=0;const p=raw3(r,new P2());out.x=p.x;out.y=p.y;out.weight=p.weight;return out;}
function transformed3(r,out){out??=new P3();const p=raw3(r);out.x=p.x+1;out.y=p.y;out.z=p.z;out.weight=p.weight;return out;}
const e2=extendsInside(.1),f2=flattenInside(.1),s2=smoothPoint(.1),t2=transformed3(.1,new P2()),e3=extendsInside(.1,new P3(0,0,7,4));
assert.deepEqual(e2.toArray(),[0,0]);assert.deepEqual(f2.toArray(),[0,0]);assert.deepEqual(s2.toArray(),[1,0]);
assert.deepEqual(t2.toArray(),[2,0]);assert.equal(t2.z,3);assert.deepEqual(e3.toArray(),[1,0,3]);assert.equal(e3.weight,4);
const vector=p=>({class:p instanceof P3?'vec3p':'vec2p',position:p.toArray(),zProperty:Object.hasOwn(p,'z')?p.z:null,weight:p.weight});
const proof={profile:'source-equation object admission/replacement; not licensed capture',extendsFresh2:vector(e2),flattenFresh2:vector(f2),smoothFresh2:vector(s2),transformInto2:vector(t2),extendsInto3:vector(e3)};
fs.writeFileSync(__dirname+'/class-buffer-vectors.json',JSON.stringify(proof,null,2)+'\n');console.log(JSON.stringify(proof));
