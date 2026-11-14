function Write-LicenseDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ProjectLicense,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Notices
    )

    $document = New-Object System.Text.StringBuilder
    [void]$document.AppendLine('# MPC Video Renderer Edition')
    [void]$document.AppendLine()
    [void]$document.AppendLine('The project is licensed under GPL version 3 or later. Third-party components retain the terms listed below.')
    [void]$document.AppendLine()

    $sections = [ordered]@{'GNU General Public License' = $ProjectLicense}
    foreach ($entry in $Notices.GetEnumerator()) {
        $sections.Add($entry.Key, $entry.Value)
    }
    foreach ($entry in $sections.GetEnumerator()) {
        [void]$document.AppendLine('## ' + $entry.Key)
        [void]$document.AppendLine()
        [void]$document.AppendLine('~~~~~~~~text')
        [void]$document.AppendLine([IO.File]::ReadAllText($entry.Value))
        [void]$document.AppendLine('~~~~~~~~')
        [void]$document.AppendLine()
    }

    $content = $document.ToString()
    if (!(Test-Path -LiteralPath $Destination -PathType Leaf) -or
        [IO.File]::ReadAllText($Destination) -cne $content) {
        [IO.File]::WriteAllText($Destination, $content, [Text.UTF8Encoding]::new($false))
    }
}
