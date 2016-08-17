var socket;
var lastTS=new Date(1970);

function batchat_setcookie(cname, cvalue, exdays) {
    var d = new Date();
    d.setTime(d.getTime() + (exdays*24*60*60*1000));
    var expires = "expires="+ d.toUTCString();
    document.cookie = cname + "=" + cvalue + "; " + expires;
}

function batchat_getcookie(cname) {
    var name = cname + "=";
    var ca = document.cookie.split(';');
    for(var i = 0; i <ca.length; i++) {
        var c = ca[i];
        while (c.charAt(0)==' ') {
            c = c.substring(1);
        }
        if (c.indexOf(name) == 0) {
            return c.substring(name.length,c.length);
        }
    }
    return "";
}

function batchat_init(targetElement, subscribedRoom, userNick, viewers_cb)
{
    var batchat_wrapper_obj = $("<div></div>").attr('id','batchat-wrapper');
    var batchat_main_obj = $("<div></div>").attr('id','batchat-main-panel');
    batchat_main_obj.append($("<div></div>").attr('id','batchat-messages-panel'));
    batchat_main_obj.append($("<div></div>").attr('id','batchat-users-panel'));
    batchat_wrapper_obj.append(batchat_main_obj);
    batchat_wrapper_obj.append($("<input></input>").attr('type','text').attr('id','batchat-bottom-bar'));
    $(targetElement).append(batchat_wrapper_obj);


    if(batchat_getcookie("batc-live-chat-username")!="")
    {
        userNick = batchat_getcookie("batc-live-chat-username");
    }

    if(userNick=='') {
        $("#batchat-bottom-bar").attr("placeholder","Type '/nick your_name' and press enter to join.");
    }
    else
    {
        $("#batchat-bottom-bar").attr("placeholder","Type a message here and press enter.");
    }
    
    $('#batchat-bottom-bar').on('keypress', function(e) {
        if (e.which == 13) {
            e.preventDefault();
            var messageText = $('#batchat-bottom-bar').val();
            $('#batchat-bottom-bar').val("");
            if(messageText.startsWith("/nick ")) {
                var wantedNick = messageText.substr(6).trim();
                userNick = wantedNick;
                batchat_setcookie("batc-live-chat-username", wantedNick, 28);
                socket.emit('setnick',{nick: userNick});
                $("#batchat-bottom-bar").attr("placeholder","Type a message here and then press enter.");
	    } else {
               if(userNick!='') {
                 socket.emit('message', {message: messageText});
               }
            }
        }
    });
    socket = io('https://beta.batc.tv', {path: "/chat/socket.io", query: 'room='+subscribedRoom});
    socket.on('history', function (data) {
        //console.log(data);
        initUsers(data.nicks);
        initHistory(data.history);
    });
    socket.on('viewers', function (data) {
        viewers_cb(data.num);
        //console.log(data);
    });
    socket.on('message', function (data) {
        //console.log(data);
        appendMsg(data);
    });
    socket.on('nicks', function (data) {
        //console.log(data);
        initUsers(data.nicks);
    });

    if(userNick!='')
    {
        socket.emit('setnick',{nick: userNick});
    }

    shuffle_panels();
}
function appendMsg(msg) {
    var ts = new Date(msg.time);
    if(ts.toLocaleDateString()!=lastTS.toLocaleDateString())
    {
        $("#batchat-messages-panel").append($("<div></div>").addClass("batchat-messages-panel-object").append($("<span></span>").addClass("batchat-message-datestamp").text(ts.toLocaleDateString())));
        lastTS=ts;
    }
    var nuMessageObj = $("<div></div>").addClass("batchat-messages-panel-object");
    nuMessageObj.append($("<span></span>").addClass("batchat-message-timestamp").text(timeString(ts)));
    nuMessageObj.append($("<span></span>").addClass("batchat-message-nick").text(msg.name));
    nuMessageObj.append($("<span></span>").addClass("batchat-message-text").text(msg.message));
    $("#batchat-messages-panel").append(nuMessageObj);
    $("#batchat-messages-panel").scrollTop($("#batchat-messages-panel")[0].scrollHeight);
}
function initHistory(history) {
    if(history.length>0)
    {
    	var dataLength = history.length;
        for (var i=0; i<dataLength; i++) {
            appendMsg(history[i]);
        }
        $("#batchat-messages-panel").scrollTop($("#batchat-messages-panel")[0].scrollHeight);
    }
}
    
function initUsers(users) {
    $("#batchat-users-panel").html("");
    if(users.length>0)
    {
    	var dataLength = users.length;
        for (var i=0; i<dataLength; i++) {
            $("#batchat-users-panel").append($("<div></div>").addClass("batchat-users-panel-object").text(users[i]));
        }
    }
}
function timeString(t) {
    var s = t.toLocaleTimeString().split(/[:]/);
    return s[0]+":"+s[1];
}
function shuffle_panels() {
    //var p_height = $(window).height() - (30+15); 
    var p_height = $("#batchat-main-panel").height() - (15);
    $("#batchat-messages-panel").height(p_height);
    $("#batchat-users-panel").height(p_height);
}
window.onresize = shuffle_panels;
