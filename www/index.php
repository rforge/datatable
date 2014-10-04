
<!-- This is the project specific website template -->
<!-- It can be changed as liked or replaced by other content -->
<!-- dummy change to tickle timestamp -->

<?php

$domain=ereg_replace('[^\.]*\.(.*)$','\1',$_SERVER['HTTP_HOST']);
$group_name=ereg_replace('([^\.]*)\..*$','\1',$_SERVER['HTTP_HOST']);
$themeroot='http://r-forge.r-project.org/themes/rforge/';

echo '<?xml version="1.0" encoding="UTF-8"?>';
?>
<!DOCTYPE HTML>
<html lang="en-US">
    <head>
        <meta charset="UTF-8">
        <meta http-equiv="refresh" content="2;url=https://github.com/Rdatatable/data.table/wiki">
        <script type="text/javascript">
            window.location.href = "https://github.com/Rdatatable/data.table/wiki"
        </script>
        <title>data.table has moved to GitHub</title>
    </head>
    <body>
        If you are not redirected automatically in 2 seconds, follow the <a href='https://github.com/Rdatatable/data.table/wiki'>link to homepage on GitHub</a>
    </body>
</html>


