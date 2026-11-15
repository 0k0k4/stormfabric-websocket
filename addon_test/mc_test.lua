-- websocket mod: microcontroller test
-- Lua block: video out -> monitor. Outputs: number 1 = messages received, bool 1 = open.
U="ws://127.0.0.1:8765"
id=nil st="?" last="" n=0 sent=0 t=0 wait=0
function onTick()
 if not websocket then st="no websocket" return end
 t=t+1
 if not id then
  if wait>0 then wait=wait-1 return end
  id,err=websocket.connect(U)
  st=id and "connecting" or ("err "..tostring(err))
  if not id then wait=300 end
  return
 end
 if t%120==0 and websocket.state(id)=="open" then
  sent=sent+1
  websocket.send(id,"mc "..sent)
 end
 while true do
  local i,k,d,x=websocket.poll(id)
  if not i then break end
  if k=="open" then st="open"
  elseif k=="message" then n=n+1 last=d
  elseif k=="error" then st="err "..tostring(x)
  elseif k=="close" then st="closed "..tostring(x) id=nil wait=300 end
 end
 output.setNumber(1,n)
 output.setBool(1,st=="open")
end
function onDraw()
 screen.setColor(0,255,0)
 screen.drawText(2,2,st)
 screen.drawText(2,10,"tx "..sent.." rx "..n)
 screen.drawText(2,18,last)
end
