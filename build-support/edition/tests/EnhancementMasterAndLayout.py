# SPDX-License-Identifier: GPL-3.0-or-later
# Extract real runtime master/strength methods; Win32 drawing is not exercised.
from pathlib import Path
import os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'Source/Edition/RendererControls.cpp').read_text()
p=(root/'Source/Edition/PropPageExtended.cpp').read_text()
r=(root/'Source/Edition/ExtendedControls.rcinc').read_text()
v=(root/'Source/VideoRenderer.cpp').read_text()
def method(name):
    start=s.index(name); a=s.index('{',start); n=1; b=a+1
    while n:
        n+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[start:b]
setint=method('std::optional<HRESULT> CMpcVideoRenderer::SetEditionInt(').split('    // One atomic MFG setting')[0]+'return {};\n}'
getint=method('std::optional<HRESULT> CMpcVideoRenderer::GetEditionInt(').split('    // Independent settings channel')[0]+'return {};\n}'
setbool=method('std::optional<HRESULT> CMpcVideoRenderer::SetEditionBool(').split('    if (!strcmp(field,"adaptiveTone"))')[0]+'return {};\n}'
getbool=method('std::optional<HRESULT> CMpcVideoRenderer::GetEditionBool(').split('    if (!strcmp(field, "adaptiveTone"))')[0]+'return {};\n}'
code=r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <optional>
#include <iostream>
#include <memory>
using HRESULT=int;using LPCSTR=const char*;
constexpr int S_OK=0,S_FALSE=1,E_INVALIDARG=-1,State_Paused=1;
struct CAutoLock{CAutoLock(int*){}};
struct Base{virtual ~Base()=default;};
struct CDX11VideoProcessor:Base{int reset=0,release=0,invalidate=0;void ResetAdaptiveTone(){++reset;}void ReleaseSvdLutRawTest(bool){++release;}void InvalidateCompletedBackBuffer(){++invalidate;}};
struct CMpcVideoRenderer{
 int m_RendererLock=0,m_filterState=State_Paused,redraw=0;
 std::atomic_int m_videoEnhancement{100},m_memoryColor{100},m_skinTone{100},m_adaptiveTone{100};
 bool m_enhancementEnabled=true; int m_requestedColorStrengths[4]{100,100,100,100};
 std::unique_ptr<Base> m_VideoProcessor=std::make_unique<CDX11VideoProcessor>();
 void Redraw(){++redraw;}void ApplyEnhancementStrengths();
 std::optional<HRESULT> SetEditionInt(LPCSTR,int),GetEditionInt(LPCSTR,int*),SetEditionBool(LPCSTR,bool),GetEditionBool(LPCSTR,bool*);
};
'''+method('void CMpcVideoRenderer::ApplyEnhancementStrengths()')+'\n'+setint+'\n'+getint+'\n'+setbool+'\n'+getbool+r'''
int main(){
 constexpr const char* keys[]={"colorEnhancementStrength","memoryColorStrength","skinToneStrength","adaptiveToneStrength"};
 unsigned cases=0;
 for(int i=0;i<101;++i) for(int mask=0;mask<16;++mask){
  CMpcVideoRenderer r;auto* dx=static_cast<CDX11VideoProcessor*>(r.m_VideoProcessor.get());
  std::atomic_int* eff[]={&r.m_videoEnhancement,&r.m_memoryColor,&r.m_skinTone,&r.m_adaptiveTone};
  int want[4];for(int j=0;j<4;++j){want[j]=(mask&(1<<j))?i:100;r.SetEditionInt(keys[j],want[j]);}
  int rel=dx->release,reset=dx->reset,invalid=dx->invalidate;
  assert(r.SetEditionBool("enhancementEnabled",false)==S_OK);
  bool enabled=true;assert(r.GetEditionBool("enhancementEnabled",&enabled)==S_OK&&!enabled);
  for(int j=0;j<4;++j){int x=-1;assert(eff[j]->load()==0&&r.GetEditionInt(keys[j],&x)==S_OK&&x==want[j]);}
  assert(dx->release==rel+(want[0]!=0)&&dx->reset==reset+(want[3]!=0));
  assert(dx->invalidate==invalid+bool(want[0]||want[1]||want[2]||want[3]));
  invalid=dx->invalidate;
  assert(r.SetEditionInt(keys[1],42).value()>=0);want[1]=42;
  assert(dx->invalidate==invalid&&eff[1]->load()==0);
  assert(r.SetEditionBool("enhancementEnabled",true)==S_OK);
  for(int j=0;j<4;++j)assert(eff[j]->load()==want[j]);
  assert(r.SetEditionInt(keys[0],101)==E_INVALIDARG&&r.SetEditionInt(keys[0],-1)==E_INVALIDARG);
  assert(r.SetEditionBool("enhancementEnabled",true)==S_FALSE);++cases;
 }
 std::cout<<"PASS "<<cases<<" extracted master/strength preservation and zero-crossing cases\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    path=Path(d)/'test.cpp';path.write_text(code);exe=Path(d)/'test'
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Static resource geometry, identity, setup and persistence contracts.
assert 'EnhancementEnabled' in v and 'm_requestedColorStrengths' in v
assert 'std::pair{L"EnhancementEnabled",enhancementEnabled}' in v
main=(root/'Source/MpcVideoRenderer.rc').read_text()
header=(root/'Source/PropPage.h').read_text()
assert 'TCM_ADJUSTRECT, FALSE' in p and 'CreateDialogParamW' in p
assert 'DS_CONTROL' in main and 'DS_CONTROL' in r and 'WS_EX_CONTROLPARENT' in main and 'WS_EX_CONTROLPARENT' in r
assert 'IDD_EDITION_RENDERER_PANE DIALOGEX' in main and 'IDD_EDITION_EXTENDED_PANE DIALOGEX' in r
# Group boxes are transparent siblings of their contained controls. Clip the
# opaque panes at the outer page, never their group interiors at pane level.
for rc, pane in ((main,'IDD_EDITION_RENDERER_PANE'), (r,'IDD_EDITION_EXTENDED_PANE')):
    pane_style=rc.split(pane+' DIALOGEX',1)[1].split('STYLE ',1)[1].splitlines()[0]
    assert 'WS_CLIPCHILDREN' not in pane_style and 'WS_CLIPSIBLINGS' in pane_style
assert 'WS_CLIPCHILDREN' in main.split('IDD_MAINPROPPAGE DIALOGEX',1)[1].split('STYLE ',1)[1].splitlines()[0]
outer=main.split('IDD_MAINPROPPAGE DIALOGEX')[1].split('END')[0]
assert 'IDC_SETTINGS_TAB' in outer and 'IDC_BUTTON1' in outer and 'IDC_EDIT2' in outer
assert 'IDC_CHECK1' not in outer and 'IDC_VIDEO_ENHANCEMENT' not in outer
assert 'IDC_EXTENDED_START_MARKER' not in p+r and 'IDC_SETTINGS_BACKGROUND' not in p
switch=p.split('void CVRMainPPage::ShowSettingsTab()')[1].split('HRESULT CVRMainPPage::OnDeactivate()')[0]
assert 'CreateDialog' not in switch and 'for(' not in switch and 'for (' not in switch
assert 'BeginDeferWindowPos' not in switch
assert switch.index('::ShowWindow(hidden, SW_HIDE)') < switch.index('::SetWindowPos(visible, HWND_TOP') < switch.index('::RedrawWindow(visible')
assert 'SWP_SHOWWINDOW' in switch and 'SWP_NOACTIVATE' in switch and 'SWP_NOZORDER' not in switch
for flag in ('RDW_INVALIDATE','RDW_ERASE','RDW_FRAME','RDW_ALLCHILDREN','RDW_UPDATENOW'):
    assert flag in switch
assert 'case WM_ERASEBKGND:' in p and '::FillRect(reinterpret_cast<HDC>(wp)' in p
assert '::GetSysColorBrush(COLOR_3DFACE)' in p
assert 'ControlParent' in header and 'm_rendererPane' in header and 'm_extendedPane' in header
assert 'case WM_COMMAND:' in p and 'case WM_HSCROLL:' in p and 'case WM_NOTIFY:' in p
assert 'GetParent(hwnd), msg, wp, lp' in p
assert 'm_showJvrOptions ? SW_SHOW : SW_HIDE' in p
assert 'TBM_SETRANGE,TRUE,MAKELONG(0,100)' in p and 'TBM_SETPAGESIZE,0,5' in p
assert 'm_oldEnhancementEnabled=m_enhancementEnabled' in p
assert 'EN_KILLFOCUS' in p and 'CommitNumericEdit(ColorLabels[i])' in p
for name in ('Processing','Display Information','Video Enhancement','VR Projection'):
    assert f'GROUPBOX        "{name}"' in r
for name in ('IDC_COLOR_ENHANCEMENT_VALUE','IDC_MEMORY_COLOR_VALUE','IDC_SKIN_TONE_VALUE','IDC_ADAPTIVE_TONE_VALUE','IDC_JVR_VERTICAL_VALUE','IDC_JVR_REPROJECTION_VALUE'):
    assert re.search(r'EDITTEXT\s+'+name+r',175,\d+,32,12',r)
assert 'IDC_VR_GROUP' in p.split('constexpr int VrIds[]')[1].split(';')[0]
print('PASS resource layout, numeric and persistence source contracts (static)')
