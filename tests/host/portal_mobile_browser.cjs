/* Optional real-browser check; uses an existing Playwright installation and local Chrome. */
const fs=require('fs'),http=require('http'),path=require('path'),os=require('os'),assert=require('assert');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||path.join(os.homedir(),'.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright'));
const root=path.resolve(__dirname,'../..'),out=process.env.PORTAL_SCREENSHOTS||path.join(root,'output/2026-11-01-wifi-no-pet');
const html=fs.readFileSync(path.join(root,'components/platform/xiaozhi/portal.html'));
const compressed=process.env.PORTAL_GZIP?fs.readFileSync(process.env.PORTAL_GZIP):require('zlib').gzipSync(html);
assert(require('zlib').gunzipSync(compressed).equals(html),'Built portal differs from source');
let state={state:'connecting',error:'none'},posts=[],statusMode='ok',scanCount=0,postStatus=202;
const malicious='家 <img src=x onerror=window.SSID_INJECTED=true> & " Wi-Fi';
const networks=Array.from({length:12},(_,i)=>({id:Buffer.from('Network '+i).toString('hex'),ssid:i===11?malicious:'附近的 Wi-Fi '+(i+1),secure:i!==10}));
const server=http.createServer(async(req,res)=>{res.setHeader('Cache-Control','no-store');res.setHeader('Content-Type','application/json');if(req.url==='/networks'){scanCount++;res.end(JSON.stringify({networks}));}else if(req.url==='/wifi/status'){if(statusMode==='drop'){req.socket.destroy();return;}res.end(JSON.stringify(state));}else if(req.url==='/wifi'){let body='';for await(const c of req)body+=c;posts.push(JSON.parse(body));res.statusCode=postStatus;res.end('{}');}else{res.setHeader('Content-Type','text/html');res.setHeader('Content-Encoding','gzip');res.end(compressed);}});
(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const url='http://127.0.0.1:'+server.address().port;
 const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE||'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',args:['--no-first-run','--no-default-browser-check']});
 const evidence=[];
 try{
  for(const size of [{width:320,height:568},{width:375,height:667},{width:390,height:844}]){
   const context=await browser.newContext({viewport:size,isMobile:true,hasTouch:true,deviceScaleFactor:1});const page=await context.newPage();const errors=[];page.on('pageerror',e=>errors.push(String(e)));
   await page.goto(url);await page.locator('.network').nth(11).click();await page.waitForFunction(()=>document.getElementById('prompt').open);
   const geometry=await page.evaluate(()=>{const p=document.getElementById('p'),b=document.getElementById('connect'),s=document.querySelector('.sheet');return {password:p.getBoundingClientRect().toJSON(),connect:b.getBoundingClientRect().toJSON(),sheet:s.getBoundingClientRect().toJSON(),focused:document.activeElement===p,ssid:document.getElementById('ssid').textContent,markup:!!document.getElementById('ssid').querySelector('img'),font:getComputedStyle(p).fontSize,width:document.documentElement.scrollWidth};});
   assert(geometry.focused);assert.equal(geometry.ssid,malicious);assert(!geometry.markup);assert.equal(await page.evaluate(()=>window.SSID_INJECTED),undefined);assert(parseFloat(geometry.font)>=16);assert(geometry.password.y>=0&&geometry.password.bottom<=size.height);assert(geometry.connect.bottom<=size.height);assert(geometry.connect.height>=44);assert(geometry.width<=size.width);
   const shot=path.join(out,`portal-mobile-${size.width}x${size.height}.png`);await page.screenshot({path:shot});evidence.push({viewport:size,...geometry,screenshot:shot});
   await page.locator('#p').fill('dummy-password');await page.locator('#connect').click();await page.locator('#status').filter({hasText:'正在连接 Wi-Fi'}).waitFor();assert(await page.locator('#scan').isDisabled());assert.equal(await page.locator('#p').inputValue(),'dummy-password');assert(!(await page.locator('#status').textContent()).includes('已连接并保存'));
   state={state:'failed',error:'auth'};await page.locator('#status').filter({hasText:'检查 Wi-Fi 密码'}).waitFor();assert.equal(await page.locator('#p').inputValue(),'dummy-password');assert(await page.locator('#connect').isEnabled());
   state={state:'connecting',error:'none'};await page.locator('#cancel').click();await page.locator('#resume').click();assert.equal(await page.locator('#p').inputValue(),'dummy-password');await page.keyboard.press('Escape');assert(!(await page.locator('#prompt').evaluate(e=>e.open)));
   await page.locator('.network').nth(10).click();assert(await page.locator('#pw').isHidden());state={state:'connected',error:'none'};await page.locator('#connect').click();await page.locator('#status').filter({hasText:'已连接并保存'}).waitFor();assert.equal(posts.at(-1).password,'');assert((await page.locator('#status').textContent()).includes('设备屏幕'));assert.deepEqual(errors,[]);
   await context.close();state={state:'connecting',error:'none'};
  }
  const context=await browser.newContext({viewport:{width:375,height:667},isMobile:true,hasTouch:true});const page=await context.newPage();await page.goto(url);await page.locator('.network').nth(11).click();await page.locator('#p').fill('dummy-password');await page.setViewportSize({width:375,height:330});await page.waitForTimeout(100);
  const keyboard=await page.evaluate(()=>{const p=document.getElementById('p').getBoundingClientRect(),b=document.getElementById('connect').getBoundingClientRect(),s=document.querySelector('.sheet');return {password:p.toJSON(),connect:b.toJSON(),sheet:s.getBoundingClientRect().toJSON(),scrollHeight:s.scrollHeight,clientHeight:s.clientHeight,vh:visualViewport.height};});assert(keyboard.password.y>=0&&keyboard.password.bottom<=330,'Password must stay visible when available height shrinks');assert(keyboard.connect.bottom<=318,'Connect must remain visible above the keyboard');await page.locator('#connect').scrollIntoViewIfNeeded();assert(await page.locator('#connect').evaluate(e=>{const r=e.getBoundingClientRect();return r.top>=0&&r.bottom<=innerHeight;}));await page.screenshot({path:path.join(out,'portal-keyboard-375x330.png')});evidence.push({keyboard});
  await page.setViewportSize({width:375,height:667});statusMode='drop';await page.locator('#connect').click();await page.locator('#check').waitFor({state:'visible',timeout:10000});assert((await page.locator('#status').textContent()).includes('尚未确认'));assert.equal(await page.locator('#p').inputValue(),'dummy-password');assert(await page.locator('#scan').isDisabled());await page.screenshot({path:path.join(out,'portal-handoff-uncertain.png')});statusMode='ok';state={state:'failed',error:'save_failed'};await page.locator('#check').click();await page.locator('#status').filter({hasText:'未能保存'}).waitFor();assert(await page.locator('#connect').isEnabled());
  await page.locator('#cancel').click();await page.locator('#manual').click();
  await page.locator('#manual-name').fill('家里的隐藏网络');await page.locator('#manual-pass').fill('test-pass');
  await page.locator('#show-pass').click();assert.equal(await page.locator('#manual-pass').getAttribute('type'),'text');
  await page.locator('#show-pass').click();await page.locator('#manual-pass').fill('');
  await page.locator('#manual-name').fill('');await page.locator('#manual-name').blur();
  await page.screenshot({path:path.join(out,'hidden-wifi-375x667.png'),fullPage:true});
  await page.locator('#manual-name').fill(' 家里的 Wi-Fi ');await page.locator('#manual-open').check();
  assert(await page.locator('#manual-pass').isHidden()); postStatus=202;state={state:'connected',error:'none'};
  await page.locator('#manual-connect').click();await page.locator('#r').filter({hasText:'已连接并保存'}).waitFor();
  assert.deepEqual(posts[posts.length-1],{ssid:' 家里的 Wi-Fi ',password:''});
  fs.writeFileSync(path.join(out,'portal-browser-evidence.json'),JSON.stringify({evidence,postCount:posts.length,scanCount,checks:'12-network list; three mobile sizes; focus; hostile SSID; input/target size; resized keyboard viewport; acceptance vs actual outcome; failure retry; open network; cancel/Escape; hotspot loss/status retry'},null,2));await context.close();console.log('portal mobile browser PASS; evidence in '+out);
 }finally{await browser.close();server.close();}
})().catch(e=>{console.error(e);server.close();process.exitCode=1;});
