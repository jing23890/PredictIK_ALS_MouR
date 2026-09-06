param([string]$Endpoint = 'http://127.0.0.1:18777/mcp')

$ErrorActionPreference = 'Stop'
$mcpHeaders = @{ Accept = 'application/json, text/event-stream' }
$mcpRequestId = 0
function Invoke-McpRequest([string]$Method, [hashtable]$Params) {
    $script:mcpRequestId++
    $body = @{ jsonrpc = '2.0'; id = $script:mcpRequestId; method = $Method; params = $Params }
    $reply = Invoke-RestMethod -Uri $Endpoint -Method Post -Headers $mcpHeaders -ContentType 'application/json' -Body ($body | ConvertTo-Json -Depth 20 -Compress) -TimeoutSec 30
    if ($reply.error) { throw ($reply.error | ConvertTo-Json -Depth 10) }
    if ($reply.result.isError) { throw ($reply.result | ConvertTo-Json -Depth 10) }
    return $reply.result
}

$init = Invoke-McpRequest 'initialize' @{ protocolVersion = '2025-03-26'; capabilities = @{}; clientInfo = @{ name = 'ALS-MCP-Verification'; version = '1.0' } }
$mcpHeaders['MCP-Protocol-Version'] = $init.protocolVersion
$notification = @{ jsonrpc = '2.0'; method = 'notifications/initialized' } | ConvertTo-Json -Compress
Invoke-WebRequest -Uri $Endpoint -Method Post -Headers $mcpHeaders -ContentType 'application/json' -Body $notification -TimeoutSec 30 | Out-Null
$toolList = Invoke-McpRequest 'tools/list' @{}
if ('unreal' -notin $toolList.tools.name) { throw 'Expected unreal tool was not advertised' }
$health = Invoke-McpRequest 'tools/call' @{ name = 'unreal'; arguments = @{ action = 'health' } }
$project = Invoke-McpRequest 'tools/call' @{ name = 'unreal'; arguments = @{ action = 'execute'; run = 'sync'; transaction = $false; commands = @(@{ kind = 'python'; mode = 'eval'; code = "{'project_file': unreal.Paths.convert_relative_path_to_full(unreal.Paths.get_project_file_path()), 'engine_version': unreal.SystemLibrary.get_engine_version()}" }) } }
[ordered]@{ server = $init.serverInfo; endpoint = $Endpoint; tools = @($toolList.tools.name); health = $health; project = $project } | ConvertTo-Json -Depth 25
