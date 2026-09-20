#!/usr/bin/env python3
"""Exercise the DEBUG artifact with Playwright in a private synthetic SQLite workspace.

Run after make DEBUG=1. Requires the optional Playwright Chromium installation.
The private evidence directory is retained for inspection; no provider is called.
"""
import hashlib,json,os,pathlib,secrets,socket,subprocess,tempfile,time,urllib.request
from playwright.sync_api import sync_playwright
root=pathlib.Path(__file__).resolve().parents[2]
state=pathlib.Path(tempfile.mkdtemp(prefix='venture-125-tenant-app-demo-'));state.chmod(0o700)
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
base=f'http://127.0.0.1:{port}'
config=state/'config.yaml';config.write_text('hosted:\n  enabled: true\n  workspace_id: 177cccd4-bdae-43c7-a77a-10c341d70e2e\n  origin: '+base+'\n');config.chmod(0o600)
env=os.environ.copy();env['VENTURE_SESSION_SECRET']=secrets.token_hex(32)
args=[str(root/'build/debug/venture'),'--config',str(config),'--database','sqlite://'+str(state/'venture.db'),'--state-dir',str(state),'--port',str(port),'--no-ai','--no-plugins','--no-automation']
def command(extra,password=None):
 p=subprocess.run(args+extra,input=password,capture_output=True,text=True,env=env,cwd=root,timeout=120)
 if p.returncode:raise RuntimeError(p.stderr)
 return p.stdout
print('Artifact SHA256: '+hashlib.sha256((root/'build/debug/venture').read_bytes()).hexdigest(),flush=True)
command(['--migrate']);command(['--tenant-admin','administrator','--tenant-password-file','-','--tenant-reason','Synthetic application demonstration'],'Synthetic-Admin-Demo-Only-125\n')
print('PASS immutable hosted workspace migrated and explicit local tenant administrator bootstrapped',flush=True)
log=open(state/'server.log','w');server=subprocess.Popen(args,env=env,cwd=root,stdout=log,stderr=subprocess.STDOUT)
try:
 for _ in range(300):
  if server.poll() is not None:raise RuntimeError('Server stopped: '+str(state))
  try:urllib.request.urlopen(base+'/api/v1/health',timeout=1).close();break
  except Exception:time.sleep(.1)
 else:raise RuntimeError('Readiness timeout')
 with sync_playwright() as p:
  browser=p.chromium.launch(headless=True);context=browser.new_context(viewport={'width':1280,'height':900},record_video_dir=str(state/'video'))
  page=context.new_page();page.goto(base+'/login');page.locator('[name=username]').fill('administrator');page.locator('[name=password]').fill('Synthetic-Admin-Demo-Only-125');page.locator('button[type=submit]').click();page.wait_for_load_state()
  r=context.request.get(base+'/api/v1/tenant_workspace');assert r.status==200,(r.status,r.text());payload=r.json();print('Workspace response shape: '+','.join(payload) if isinstance(payload,dict) else 'Workspace array',flush=True)
  rows=payload.get('data',payload) if isinstance(payload,dict) else payload
  if isinstance(rows,dict):rows=rows.get('items',rows.get('records',rows))
  assert isinstance(rows,list),(type(rows),rows);workspace=rows[0]['id']
  page.goto(base+'/e/tenant_workspace');page.wait_for_timeout(900)
  assert context.request.get(base+'/settings').status==403
  assert context.request.get(base+'/api/v1/user').status==403
  print('PASS interactive tenant administrator reaches workspace records; host settings and global user CRUD return 403',flush=True)
  def action(kind,ident,name,values):
   response=context.request.post(base+f'/api/v1/{kind}/{ident}/actions/{name}',data=values)
   assert response.ok,(response.status,response.text());return response.json()
  action('tenant_workspace',workspace,'set_state',{'state':'suspended','reason':'Demonstrate suspended business boundary'})
  assert context.request.get(base+'/api/v1/invoice').status==403
  assert context.request.get(base+'/api/v1/health').status==200
  page.goto(base+'/e/tenant_membership');assert page.locator('body').inner_text();page.wait_for_timeout(900)
  print('PASS generic audited action suspends business access while health and membership review remain available',flush=True)
  action('tenant_workspace',workspace,'set_state',{'state':'active','reason':'Demonstrate explicit tenant reactivation'})
  assert context.request.get(base+'/api/v1/invoice').status==200
  print('PASS explicit tenant reactivation restores business access',flush=True)
  invitation=action('tenant_invitation',0,'invite',{'organization_id':1,'organization_role':'editor','role':'member','lifetime_seconds':600,'reason':'Explicit synthetic colleague invitation'})
  colleague=browser.new_context();cp=colleague.new_page();cp.goto(base+'/account/invitation')
  cp.locator('[name=capability]').fill(invitation['capability']);cp.locator('[name=username]').fill('retained-colleague');cp.locator('[name=password]').fill('Synthetic-Member-Old-Only-125');cp.locator('button[type=submit]').click()
  cp.wait_for_load_state();assert 'Invitation accepted' in cp.locator('body').inner_text(), cp.locator('body').inner_text()
  memberships=context.request.get(base+'/api/v1/tenant_membership').json()['records'];member=next(row for row in memberships if row['name']=='retained-colleague');retained_id=member['user_id']
  print('PASS explicit one-time invitation creates an ordinary member with a retained local identity',flush=True)
  colleague.close();server.terminate();server.wait(timeout=15)
  command(['--tenant-revoke-credentials','--tenant-reason','Synthetic restored-authority quarantine'])
  command(['--tenant-admin','administrator','--tenant-password-file','-','--tenant-recover','--tenant-reason','Synthetic reviewing administrator recovery'],'Synthetic-Fresh-Admin-Only-125\n')
  server=subprocess.Popen(args,env=env,cwd=root,stdout=log,stderr=subprocess.STDOUT)
  for _ in range(300):
   try:urllib.request.urlopen(base+'/api/v1/health',timeout=1).close();break
   except Exception:time.sleep(.1)
  context.clear_cookies();page.goto(base+'/login');page.locator('[name=username]').fill('administrator');page.locator('[name=password]').fill('Synthetic-Fresh-Admin-Only-125');page.locator('button[type=submit]').click();page.wait_for_load_state()
  recovery=action('tenant_membership',member['id'],'invite_recovery',{'lifetime_seconds':600,'reason':'Reviewed ordinary member recovery'})
  colleague=browser.new_context();cp=colleague.new_page();cp.goto(base+'/account/invitation');cp.locator('[name=capability]').fill(recovery['capability']);cp.locator('[name=username]').fill('retained-colleague');cp.locator('[name=password]').fill('Synthetic-Fresh-Member-Only-125');cp.locator('button[type=submit]').click();cp.wait_for_load_state();assert 'Invitation accepted' in cp.locator('body').inner_text(), cp.locator('body').inner_text()
  memberships=context.request.get(base+'/api/v1/tenant_membership').json()['records'];member=next(row for row in memberships if row['name']=='retained-colleague');assert member['user_id']==retained_id and member['role']=='member' and member['active']
  assert context.request.get(base+'/api/v1/invoice').status==403
  print('PASS stopped restore quarantine and named administrator recovery preserve suspension; targeted member recovery preserves user ID and member role',flush=True)
  page.goto(base+'/e/tenant_membership');page.wait_for_timeout(1200);page.screenshot(path=str(state/'final.png'),full_page=True)
  colleague.close();video=page.video;context.close();print('Recording: '+video.path(),flush=True);browser.close()
finally:
 server.terminate()
 try:server.wait(timeout=15)
 except subprocess.TimeoutExpired:server.kill();server.wait()
 log.close();print('Evidence directory: '+str(state),flush=True)
