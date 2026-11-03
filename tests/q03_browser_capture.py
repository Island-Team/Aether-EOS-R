#!/usr/bin/env python3
"""Capture Q03 loaded browser responses and rendered Vision JPEG with Firefox."""

import argparse
import http.client
import json
from pathlib import Path
import time
from urllib.parse import urljoin, urlsplit

from q01_selection import load_selection
from q03_recording_proxy import RecordingProxy


FETCH_BYTES = """
const [url, done] = arguments;
fetch(url, {cache: 'no-store'}).then(async response => {
  const bytes = new Uint8Array(await response.arrayBuffer());
  let raw = '';
  for (let offset = 0; offset < bytes.length; offset += 16384) {
    raw += String.fromCharCode(...bytes.subarray(offset, offset + 16384));
  }
  done({url: response.url, status: response.status, body_base64: btoa(raw)});
}).catch(error => done({error: String(error)}));
"""

CONTROL_READY = """/* Q03_CONTROL_READY */
const revision=arguments[0];
const nav=document.querySelector('.isos-workbench__nav [aria-label="Pages"] button:nth-child(2)');
const card=document.querySelector('#eos-controls .eos-control__card--typed');
const visible=element => Boolean(element && element.getClientRects().length &&
  getComputedStyle(element).visibility !== 'hidden');
const binding=card && Array.from(card.querySelectorAll('code'))
  .map(item => item.textContent || '').find(value => value.startsWith('eos/'));
const invoke=card?.querySelector('button.eos-control__primary');
const active=Array.from(document.querySelectorAll('.isos-workbench__safety code'))
  .some(item => item.textContent === revision);
return nav?.getAttribute('aria-current') === 'page' &&
  new URL(window.location.href).searchParams.get('page') === 'eos' &&
  visible(card) && visible(document.querySelector('#eos-controls')) &&
  active && binding && visible(invoke) && !invoke.disabled
    ? {binding_path:binding,revision} : null;
"""


def wait_browser(driver, script, *args, timeout=30):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        observed=driver.execute_script(script,*args)
        if observed:
            return observed
        time.sleep(.1)
    raise AssertionError('browser control state did not become ready')


def capture_assets(driver, proxy, label, url, files, *, hosted=False,
                   interact=False, vision=False, revision=None):
    parsed=urlsplit(url)
    origin=f'{parsed.scheme}://{parsed.netloc}'
    prefix=origin+('/isosui/' if hosted else '/')
    allowed={url}
    for name in files:
        if name == 'manifest.json' or name.endswith(('.js','.css')):
            allowed.add(urljoin(prefix,name))
    assert 'manifest.json' in files
    proxy.capture(label,allowed)
    driver.get(url)
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        loaded = driver.execute_script("""
          return Boolean(document.querySelector('main.node-graph-manager '
            + '.node-graph-canvas .react-flow__pane'));
        """)
        if loaded:
            break
        time.sleep(.2)
    else:
        raise AssertionError(f'editor DOM did not load: {url}')
    control=None
    if interact:
        assert revision is not None, 'selected revision required for Control read'
        nav=driver.find_elements('css selector',
            '.isos-workbench__nav [aria-label="Pages"] button')
        assert len(nav)==3, 'Graph/EOS/EGIA navigation missing'
        nav[1].click()
        ready=wait_browser(driver,CONTROL_READY,revision)
        control={'url':driver.current_url,'dom_html':driver.execute_script(
            'return document.documentElement.outerHTML'),**ready}
        driver.find_elements('css selector',
            '.isos-workbench__nav [aria-label="Pages"] button')[0].click()
        wait_browser(driver,"""return Boolean(
          document.querySelector('.isos-workbench__nav [aria-label="Pages"] button:first-child')
            ?.getAttribute('aria-current') === 'page' &&
          document.querySelector('main.node-graph-manager .node-graph-canvas .react-flow__pane'));
        """)
    if vision:
        open_vision(driver)
    manifest_url=urljoin(prefix,'manifest.json')
    manifest=driver.execute_async_script(FETCH_BYTES,manifest_url)
    assert set(manifest)=={'url','status','body_base64'} and manifest['status']==200
    driver.execute_async_script("""const done=arguments[arguments.length-1];
      requestAnimationFrame(() => requestAnimationFrame(() => done(true)));""")
    quiet_since=time.monotonic()
    previous=None
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        current=driver.execute_script("""return performance.getEntriesByType('resource')
          .filter(x => /\\.(js|css)$/.test(new URL(x.name).pathname))
          .map(x => x.name).sort();""")
        if current != previous:
            previous=current
            quiet_since=time.monotonic()
        if time.monotonic()-quiet_since >= 1.5:
            break
        time.sleep(.1)
    else:
        raise AssertionError('browser JS/CSS resource set did not settle')
    provenance = driver.execute_script("""
      return {navigation: performance.getEntriesByType('navigation').map(x => x.name),
        scripts: Array.from(document.scripts).filter(x => x.src).map(x => x.src),
        resources: performance.getEntriesByType('resource')
          .filter(x => /\\.(js|css)$/.test(new URL(x.name).pathname))
          .map(x => ({url: x.name, initiator_type: x.initiatorType}))};
    """)
    provenance['manifest_fetch']=manifest
    responses = proxy.finish()
    return {'url':url,'dom_html':driver.execute_script(
        'return document.documentElement.outerHTML'),
        'responses':responses,'provenance':provenance,'control':control}


def open_vision(driver):
    from selenium.webdriver.common.by import By
    from selenium.webdriver.support.ui import WebDriverWait
    button = WebDriverWait(driver, 30).until(lambda browser: browser.find_element(
        By.XPATH, "//div[contains(@class,'isos-workbench__panel-menu')]"
                  "//button[normalize-space()='Vision']"))
    assert button.get_attribute('aria-pressed') == 'false', 'Vision panel was already open'
    button.click()
    assert button.get_attribute('aria-pressed') == 'true', 'Vision panel did not open'


def capture_vision(driver, url, revision, ui_commit):
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        snapshot = driver.execute_script("""
          const image = document.querySelector('.eos-vision__image img[src^="blob:"]');
          return image ? {src: image.src, html: document.documentElement.outerHTML} : null;
        """)
        if snapshot:
            response = driver.execute_async_script(FETCH_BYTES, snapshot['src'])
            if set(response) == {'url','status','body_base64'} and response['status'] == 200:
                from q01_selection import sha256
                import base64
                digest=sha256(base64.b64decode(response['body_base64'],validate=True))
                if digest in snapshot['html']:
                    return {'url':url,'active_revision':revision,
                            'source_commit':ui_commit,'dom_html':snapshot['html'],
                            'image_base64':response['body_base64']}
        time.sleep(.2)
    raise AssertionError('browser Vision image and displayed digest did not agree')


def capture_matching_vision(driver, root, ready, ui_commit):
    from q01_full_replay import capture_selected_vision
    from q01_selection import sha256
    import base64
    for _ in range(16):
        capture_selected_vision(root,Path(ready['isos_home']),ready['run_id'],
            ready['revision'],ready['bridge_port'],urlsplit(ready['url']).port)
        owner=json.loads((root/'q03-vision-owner.json').read_text())
        vision=capture_vision(driver,ready['url'],ready['revision'],ui_commit)
        digest=sha256(base64.b64decode(vision['image_base64'],validate=True))
        if digest == owner['metadata']['jpeg_sha256']:
            from q01_browser_raw import verify_vision_dom
            try:
                verify_vision_dom(vision['dom_html'],owner['metadata'])
            except AssertionError:
                continue
            return vision
    raise AssertionError('browser Vision did not share a fresh owner/bridge/EGIA frame')


def firefox_options(proxy_port, visible=False):
    from selenium.webdriver.firefox.options import Options
    options=Options()
    if not visible:
        options.add_argument('-headless')
    options.set_preference('network.proxy.type',1)
    options.set_preference('network.proxy.http','127.0.0.1')
    options.set_preference('network.proxy.http_port',proxy_port)
    options.set_preference('network.proxy.no_proxies_on','')
    options.set_preference('network.proxy.allow_hijacking_localhost',True)
    options.set_preference('browser.cache.disk.enable',False)
    options.set_preference('browser.cache.memory.enable',False)
    return options


def direct_synthetic_origin_denials(ready, synthetic_origin):
    """Observe both unmodified owner listeners rejecting the synthetic site."""
    checks=[('isos',ready['api_port'],'GET','/api/isos/v1/process-status'),
            ('eos',ready['bridge_port'],'OPTIONS','/api/eos/v2/control')]
    rows=[]
    for owner,port,method,path in checks:
        connection=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
        headers={'Origin':synthetic_origin}
        if method == 'OPTIONS':
            headers['Access-Control-Request-Method']='POST'
            headers['Access-Control-Request-Headers']='content-type'
        try:
            connection.request(method,path,headers=headers)
            response=connection.getresponse()
            row={'owner':owner,'method':method,'path':path,
                 'origin':synthetic_origin,'status':response.status,
                 'access_control_allow_origin':response.getheader(
                     'Access-Control-Allow-Origin')}
            response.read()
        finally:
            connection.close()
        assert row['status'] == 403 and row['access_control_allow_origin'] is None, row
        rows.append(row)
    return rows


def eos_post_preflight_seen(rows, control_url):
    return any(row['method'] == 'OPTIONS' and row['url'] == control_url and
               row['preflight_request_headers'].get('access-control-request-method') == 'POST' and
               row['preflight_request_headers'].get('access-control-request-headers','').lower() ==
                   'content-type' and row['raw_status'] == 204
               for row in rows)


def probe_browser_eos_preflight(driver, control_url):
    result=driver.execute_async_script("""const [url, done] = arguments;
      fetch(url, {method:'POST', mode:'cors', credentials:'omit', cache:'no-store',
        targetAddressSpace:'loopback', headers:{'Content-Type':'application/json'},
        body:JSON.stringify({op:'time'})})
        .then(response => done({url:response.url,status:response.status}))
        .catch(error => done({error:String(error)}));""", control_url)
    assert result == {'url':control_url,'status':200}, result
    return {'mode':'explicit-browser-fetch',**result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive',required=True,type=Path)
    parser.add_argument('--selection',required=True,type=Path)
    parser.add_argument('--expect-selection-sha256',required=True)
    parser.add_argument('--visible',action='store_true')
    args=parser.parse_args()
    selection=load_selection(args.selection,args.expect_selection_sha256)
    root=args.archive.resolve()
    ready=json.loads((root/'browser-ready.json').read_text())
    build=json.loads((root/'q03-build-receipt.json').read_text())
    assert build['selection_sha256']==args.expect_selection_sha256
    from selenium import webdriver
    hosted_port=urlsplit(ready['hosted_urls']['en']).port
    assert hosted_port == urlsplit(ready['hosted_urls']['cn']).port
    proxy=RecordingProxy(hosted_origin=f'http://127.0.0.1:{hosted_port}',
                         isos_port=ready['api_port'],
                         eos_port=ready['bridge_port']).start()
    direct_denials=direct_synthetic_origin_denials(ready,proxy.hosted_origin)
    driver=webdriver.Firefox(options=firefox_options(proxy.port,args.visible))
    try:
        driver.set_script_timeout(30)
        local_files={name:digest for name,digest in build['ui_local_sha256'].items()
                     if name in ('index.html','manifest.json') or name.endswith(('.js','.css'))}
        hosted_files={name:digest for name,digest in build['ui_hosted_sha256'].items()
                      if name in ('index.html','manifest.json') or name.endswith(('.js','.css'))}
        local=capture_assets(driver,proxy,'local',ready['url'],local_files,
                             interact=True,vision=True,revision=ready['revision'])
        vision=capture_matching_vision(driver,root,ready,
            selection['owners']['ui']['commit'])
        hosted={language:capture_assets(driver,proxy,f'hosted.{language}',
                                        ready['hosted_urls'][language],hosted_files,
                                        hosted=True,interact=True,revision=ready['revision'])
                for language in ('en','cn')}
        control_url=f"http://127.0.0.1:{ready['bridge_port']}/api/eos/v2/control"
        browser_probe={'mode':'ui-navigation'}
        if not eos_post_preflight_seen(proxy.cors_rows,control_url):
            browser_probe=probe_browser_eos_preflight(driver,control_url)
        assert eos_post_preflight_seen(proxy.cors_rows,control_url), \
            'hosted browser did not issue EOS POST/content-type preflight through proxy'
        (root/'q03-simulated-hosted-origin.json').write_text(json.dumps({
            'label':'simulated hosted-Origin browser evidence',
            'synthetic_origin':proxy.hosted_origin,
            'approved_forwarded_origin':'https://os-island.com',
            'destination_ports':{'isos':ready['api_port'],
                                 'eos':ready['bridge_port']},
            'direct_without_translation':direct_denials,
            'browser_probe':browser_probe,
            'requests':proxy.cors_rows,
        },sort_keys=True,ensure_ascii=False)+'\n')
        (root/'q03-browser-raw.json').write_text(json.dumps(
            {'local':local,'hosted':hosted,'vision':vision},sort_keys=True,
            ensure_ascii=False)+'\n')
        from q01_browser_raw import verify_browser_raw
        proof=verify_browser_raw(root,local_files,hosted_files,ready['revision'],
                                 selection['owners']['ui']['commit'])
        hosted_observation={language:{
            'path':f'/{language}/isos','editor_loaded':True,
            'source_commit':selection['owners']['ui']['commit'],
            'index_sha256':proof['hosted'][language]['index.html'],
            'script_sha256':{name:digest for name,digest in
                             proof['hosted'][language].items() if name.endswith('.js')},
        } for language in ('en','cn')}
        (root/'q03-hosted-browser-observation.json').write_text(json.dumps(
            hosted_observation,sort_keys=True,ensure_ascii=False)+'\n')
        owner=json.loads((root/'q03-vision-owner.json').read_text())
        (root/'q03-browser-vision-observation.json').write_text(json.dumps({
            'metadata':owner['metadata'],
            'jpeg_verified_sha256':proof['vision_jpeg_sha256'],
            'rendered':True,'active_revision':ready['revision'],
            'source_commit':selection['owners']['ui']['commit'],
        },sort_keys=True,ensure_ascii=False)+'\n')
    finally:
        driver.quit()
        proxy.close()


if __name__=='__main__':
    main()
