const fs=require('fs'),vm=require('vm'),assert=require('assert');
const context={YYRef:class{},Long:class{},g_pBuiltIn:{pointer_null:{}},g_incQuotesSTRING_RValue:0};
vm.createContext(context);vm.runInContext(fs.readFileSync(__dirname+'/yyTypes.js','utf8'),context);
const pairs=[0,-0,.001,.004,.005,.0078125,1,1.001,1.004,1.005,2147483647,2147483648,1e21];
const vectors=pairs.map(x=>({value:x,key:context.yyGetString(x)}));
assert.equal(context.yyGetString(.001),context.yyGetString(.004));
assert.notEqual(context.yyGetString(0),context.yyGetString(.001));
fs.writeFileSync(__dirname+'/key-vectors.json',JSON.stringify(vectors,null,2)+'\n');
console.log(JSON.stringify(vectors));
