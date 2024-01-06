# ngx_http_var_limit_req_module

This is a derived module from [Module ngx_http_limit_req_module](http://nginx.org/en/docs/http/ngx_http_limit_req_module.html).
It allows override values of rate, burst, dry_run, and status with variables.
It also provides two directives for monitoring.

## Table of Contents

* [Example Configuration](#example-configuration)
* [Directives](#directives)
    * [var_limit_req](#var_limit_req)
    * [var_limit_req_dry_run](#var_limit_req_dry_run)
    * [var_limit_req_log_level](#var_limit_req_log_level)
    * [var_limit_req_status](#var_limit_req_status)
    * [var_limit_req_zone](#var_limit_req_zone)
    * [var_limit_req_top](#var_limit_req_top)
    * [var_limit_req_monitor](#var_limit_req_monitor)
* [Embedded Variables](#embedded-variables)

## Example Configuration

```
http {
    map $host $host_group {
        default            1;
        www1.example.com   2;
    }

    map $request_method $method_group {
        default            B;
        GET                A;
        HEAD               A;
        OPTIONS            A;
    }

    map $host_group-$method_group $limit_req_by_host_rate {
        default       10000r/s;
        1-A           10000r/s;
        1-B            1000r/s;
        2-A             500r/s;
        2-B              50r/s;
    }

    map $host_group-$method_group $limit_req_by_host_burst {
        default        2000;
        1-A            2000;
        1-B             200;
        2-A             100;
        2-B              10;
    }

    map $host_group $limit_req_by_host_status {
        default       429;
        2             444;
    }

    var_limit_req_zone $host-$method_group zone=limit_req_by_host_zone:10m
        rate_var=$limit_req_by_host_rate burst_var=$limit_req_by_host_burst
        status_var=$limit_req_by_host_status;

    ...

    server {

        ...

        var_limit_req_log_level info;

        location /download/ {
            var_limit_req zone=limit_req_by_host_zone nodelay;
            var_limit_req_status 429;
        }
    }

    server {
        listen       81;
        server_name  localhost;

        allow 127.0.0.1;
        deny  all;

        location = /top-limit-req {
            var_limit_req_top limit_req_by_host_zone default_n=3;
        }

        location = /status-limit-req {
            var_limit_req_monitor limit_req_by_host_zone;
        }
    }
}    
```

## Directives

### var_limit_req

* Syntax: **var_limit_req** zone=*name* [burst=*number*] [nodelay | delay=*number*];
* Default: -
* Context: http, server, location

### var_limit_req_dry_run

* Syntax: **var_limit_req_dry_run** on | off;
* Default: var_limit_req_dry_run off;
* Context: http, server, location

### var_limit_req_log_level

* Syntax: **var_limit_req_log_level** info | notice | warn | error;
* Default: var_limit_req_log_level error;
* Context: http, server, location

### var_limit_req_status

* Syntax: **var_limit_req_status** *code*;
* Default: limit_req_status 503;
* Context: http, server, location

### var_limit_req_zone

* Syntax: **var_limit_req_zone** key zone=*name:size* rate=*rate* [sync] rate_var=*$var_name* burst_var=*$var_name* dry_run_var=*$var_name* status_var=*$var_name*;
* Default: -
* Context: http

### var_limit_req_top

* Syntax: **var_limit_req_top** *zone* default_n=*number*;
* Default: -
* Context: location

It shows the top n items in the shared memory zone.
You can override n with a query parameter.

> [!CAUTION]
> Avoid calling this method on dictionaries with a very large number of keys as it may lock the dictionary for significant amount of time and block Nginx worker processes trying to access the dictionary.

The example:

```
$ curl -sSD - 'http://localhost:81/top-limit-req?n=5'
HTTP/1.1 200 OK
Server: nginx/1.25.4
Date: Sat, 06 Jan 2024 13:45:26 GMT
Content-Type: text/plain
Content-Length: 690
Connection: keep-alive
x-num-all-keys: 6
x-top-n: 5

key:www1.example.com-B  adjusted_excess:0.000   raw_excess:9.950        rate:50 burst:10        last:185339209    last_time:Sat, 06 Jan 2024 13:45:24 GMT
key:www5.example.com-A  adjusted_excess:0.000   raw_excess:0.000        rate:10000      burst:2000      last:185339176    last_time:Sat, 06 Jan 2024 13:45:24 GMT
key:www4.example.com-A  adjusted_excess:0.000   raw_excess:0.000        rate:10000      burst:2000      last:185339167    last_time:Sat, 06 Jan 2024 13:45:24 GMT
key:www3.example.com-A  adjusted_excess:0.000   raw_excess:0.000        rate:10000      burst:2000      last:185339159    last_time:Sat, 06 Jan 2024 13:45:24 GMT
key:www2.example.com-A  adjusted_excess:0.000   raw_excess:0.000        rate:10000      burst:2000      last:185339151    last_time:Sat, 06 Jan 2024 13:45:24 GMT
```

### var_limit_req_monitor

* Syntax: **var_limit_req_monitor** *zone*;
* Default: -
* Context: location

It shows the items in the shared memory zone corresponding keys specified with
the comma separated values in the query paramter "key".

The non existing keys are silently skipped and only existing keys are shown.

The example:

```
$ curl -sSD - http://localhost:81/status-limit-req?key=www1.example.com-A,www1.example.com-B
HTTP/1.1 200 OK
Server: nginx/1.25.4
Date: Sat, 06 Jan 2024 13:45:26 GMT
Content-Type: text/plain
Content-Length: 270
Connection: keep-alive

key:www1.example.com-A  adjusted_excess:0.000   raw_excess:0.000        rate:500        burst:100       last:185339143    last_time:Sat, 06 Jan 2024 13:45:24 GMT
key:www1.example.com-B  adjusted_excess:0.000   raw_excess:9.950        rate:50 burst:10        last:185339209    last_time:Sat, 06 Jan 2024 13:45:24 GMT
```

## Embedded Variables

* $var_limit_req_status
    * keeps the result of limiting the request processing rate: `PASSED`, `DELAYED`, `REJECTED`, `DELAYED_DRY_RUN`, or `REJECTED_DRY_RUN` 
