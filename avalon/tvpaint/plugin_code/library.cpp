#include <winsock2.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <fstream>
#include "plugdllx.h"
#include "plugx.h"

#include <boost/chrono.hpp>

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#include <map>
#include <string>
#include <queue>

#include <pthread.h>

#include "json.hpp"
#include "jsonrpcpp.hpp"

// All functions not exported should be static.
// All global variables should be static.

#define DEBUG_MSG(str) do { std::cout << str << std::endl; } while( false )

static struct
// mReq Identification of the requester.  (=0 closed, !=0 requester ID)
{DWORD mReq;} Data = {0};
PIFilter *current_filter;

jsonrpcpp::Parser parser;

typedef websocketpp::client<websocketpp::config::asio_client> client;


int client_request_id = 1;
std::queue<std::string> messages;
std::map<int, jsonrpcpp::Response> responses;


class connection_metadata {
private:
    websocketpp::connection_hdl m_hdl;
    client *m_endpoint;
    std::string m_status;
    std::string m_uri;
    std::string m_server;
    std::string m_error_reason;
public:
    typedef websocketpp::lib::shared_ptr<connection_metadata> ptr;

    connection_metadata(websocketpp::connection_hdl hdl, std::string uri, client *endpoint)
            : m_hdl(hdl), m_status("Connecting"), m_uri(uri), m_server("N/A") {
        m_endpoint = endpoint;
    }

    void on_open(client *c, websocketpp::connection_hdl hdl) {
        m_status = "Open";

        client::connection_ptr con = c->get_con_from_hdl(hdl);
        m_server = con->get_response_header("Server");
    }

    void on_fail(client *c, websocketpp::connection_hdl hdl) {
        m_status = "Failed";

        client::connection_ptr con = c->get_con_from_hdl(hdl);
        m_server = con->get_response_header("Server");
        m_error_reason = con->get_ec().message();
    }

    void on_close(client *c, websocketpp::connection_hdl hdl) {
        m_status = "Closed";
        client::connection_ptr con = c->get_con_from_hdl(hdl);
        std::stringstream s;
        s << "close code: " << con->get_remote_close_code() << " ("
          << websocketpp::close::status::get_string(con->get_remote_close_code())
          << "), close reason: " << con->get_remote_close_reason();
        m_error_reason = s.str();
    }

    void on_message(websocketpp::connection_hdl, client::message_ptr msg) {
        std::string json_str;
        if (msg->get_opcode() == websocketpp::frame::opcode::text) {
            json_str = msg->get_payload();
        } else {
            json_str = websocketpp::utility::to_hex(msg->get_payload());
        }
        process_message(json_str);
    }

    void ping(nlohmann::json entity) {
        // add seqeunce to result
        entity["result"]["seq"] = entity["params"]["seq"];

        // remove params and method keys
        nlohmann::json::iterator params_it = entity.find("params");
        nlohmann::json::iterator method_it = entity.find("method");
        entity.erase(method_it);
        entity.erase(params_it);

        // send response
        send(entity.dump());
    }

    void process_message(std::string msg) {
        std::cout << "--> " << msg << "\n";
        try {
            // Make sure message has jsonrpc key (as Python's module `wsrpc_aiohttp` does not send it)
            jsonrpcpp::entity_ptr entity = parser.do_parse(msg);
            if (!entity) {
                // Return error code?

            } else if (entity->is_response()) {
                jsonrpcpp::Response response = jsonrpcpp::Response(entity->to_json());
                responses[response.id().int_id()] = response;

            } else if (entity->is_request() || entity->is_notification()) {
                messages.push(msg);
            }
        }
        catch (const jsonrpcpp::RequestException &e) {
            std::string message = e.to_json().dump();
            std::cout << "<-- " << e.to_json().dump() << "\n";
            send(message);
        }
        catch (const jsonrpcpp::ParseErrorException &e) {
            std::string message = e.to_json().dump();
            std::cout << "<-- " << message << "\n";
            send(message);
        }
        catch (const jsonrpcpp::RpcException &e) {
            std::cerr << "RpcException: " << e.what() << "\n";
            std::string message = jsonrpcpp::ParseErrorException(e.what()).to_json().dump();
            std::cout << "<-- " << message << "\n";
            send(message);
        }
        catch (const std::exception &e) {
            std::cerr << "Exception: " << e.what() << "\n";
        }
    }

    void send(std::string message) {
        websocketpp::lib::error_code ec;

        m_endpoint->send(m_hdl, message, websocketpp::frame::opcode::text, ec);
        if (ec) {
            std::cout << "> Error sending message: " << ec.message() << std::endl;
            return;
        }
    }

    void send_notification(jsonrpcpp::Notification *notification) {
        send(notification->to_json().dump());
    }

    void send_response(jsonrpcpp::Response *response) {
        send(response->to_json().dump());
    }

    void send_request(jsonrpcpp::Request *request) {
        send(request->to_json().dump());
    }

    websocketpp::connection_hdl get_hdl() const {
        return m_hdl;
    }

    std::string get_status() const {
        return m_status;
    }
};


class websocket_endpoint {
private:
    client m_endpoint;
    connection_metadata::ptr client_metadata;
    websocketpp::lib::shared_ptr<websocketpp::lib::thread> m_thread;
public:
    websocket_endpoint() {
        m_endpoint.clear_access_channels(websocketpp::log::alevel::all);
        m_endpoint.clear_error_channels(websocketpp::log::elevel::all);
    }

    ~websocket_endpoint() {
        close_connection();
    }

    void close_connection() {
        m_endpoint.stop_perpetual();

        // Only client connection
        if (client_metadata->get_status() == "Open") {
            std::cout << "> Closing connection" << std::endl;

            websocketpp::lib::error_code ec;
            m_endpoint.close(client_metadata->get_hdl(), websocketpp::close::status::going_away, "", ec);
            if (ec) {
                std::cout << "> Error closing connection: " << ec.message() << std::endl;
            }
        }
        m_thread->join();
    }

    connection_metadata::ptr get_client_metadata() {
        return client_metadata;
    }

    int connect(std::string const &uri) {
        m_endpoint.init_asio();
        m_endpoint.start_perpetual();

        m_thread.reset(new websocketpp::lib::thread(&client::run, &m_endpoint));

        websocketpp::lib::error_code ec;

        if (client_metadata && client_metadata->get_status() == "Open") {
            std::cout << "> Already connected" << std::endl;
            return 0;
        }

        client::connection_ptr con = m_endpoint.get_connection(uri, ec);

        if (ec) {
            std::cout << "> Connect initialization error: " << ec.message() << std::endl;
            return -1;
        }

        client_metadata = websocketpp::lib::make_shared<connection_metadata>(con->get_handle(), uri, &m_endpoint);

        con->set_open_handler(websocketpp::lib::bind(
                &connection_metadata::on_open,
                client_metadata,
                &m_endpoint,
                websocketpp::lib::placeholders::_1
        ));
        con->set_fail_handler(websocketpp::lib::bind(
                &connection_metadata::on_fail,
                client_metadata,
                &m_endpoint,
                websocketpp::lib::placeholders::_1
        ));
        con->set_close_handler(websocketpp::lib::bind(
                &connection_metadata::on_close,
                client_metadata,
                &m_endpoint,
                websocketpp::lib::placeholders::_1
        ));
        con->set_message_handler(websocketpp::lib::bind(
                &connection_metadata::on_message,
                client_metadata,
                websocketpp::lib::placeholders::_1,
                websocketpp::lib::placeholders::_2
        ));

        m_endpoint.connect(con);

        return 1;
    }

    void close(websocketpp::close::status::value code, std::string reason) {
        if (!client_metadata || client_metadata->get_status() != "Open") {
            std::cout << "> Not connected yet" << std::endl;
            return;
        }

        websocketpp::lib::error_code ec;

        m_endpoint.close(client_metadata->get_hdl(), code, reason, ec);
        if (ec) {
            std::cout << "> Error initiating close: " << ec.message() << std::endl;
        }
    }

    void send(std::string message) {
        if (!client_metadata || client_metadata->get_status() != "Open") {
            std::cout << "> Not connected yet" << std::endl;
            return;
        }

        client_metadata->send(message);
    }

    void send_notification(jsonrpcpp::Notification *notification) {
        client_metadata->send_notification(notification);
    }

    void send_response(jsonrpcpp::Response *response) {
        client_metadata->send(response->to_json().dump());
    }

    void send_response(std::shared_ptr<jsonrpcpp::Entity> response) {
        client_metadata->send(response->to_json().dump());
    }

    void send_request(jsonrpcpp::Request *request) {
        client_metadata->send_request(request);
    }

    connection_metadata::ptr get_metadata() const {
        return client_metadata;
    }
};

class Communicator {
private:
    // URL to websocket server
    std::string websocket_url;
    // Is communicator connected to server
    // - may be used to reconnect if needed (not implemented)
    bool connected;
    // Should be avalon plugin available?
    // - this may change during processing if websocketet url is not set or server is down
    bool use_avalon;
public:
    Communicator();
    websocket_endpoint endpoint;
    bool is_connected();
    bool is_usable();
    void connect();
    void process_requests();
    jsonrpcpp::Response call_method(std::string method_name, nlohmann::json params);
    void call_notification(std::string method_name, nlohmann::json params);
};

Communicator::Communicator() {
    // URL to websocket server
    websocket_url = std::getenv("WEBSOCKET_URL");
    // Is communicator connected to server
    // - may be used to reconnect if needed (not implemented)
    connected = false;
    // Should be avalon plugin available?
    // - this may change during processing if websocketet url is not set or server is down
    if (websocket_url == "") {
        use_avalon = false;
    } else {
        use_avalon = true;
    }
}

bool Communicator::is_connected(){
    return connected;
}

bool Communicator::is_usable(){
    return use_avalon;
}

void Communicator::connect()
{
    if (!use_avalon) {
        return;
    }
    int con_result;
    con_result = endpoint.connect(websocket_url);
    if (con_result == -1)
    {
        connected = false;
        use_avalon = false;
    } else {
        connected = true;
        use_avalon = true;
    }
}

void Communicator::call_notification(std::string method_name, nlohmann::json params) {
    if (!use_avalon || !connected) {return;}

    jsonrpcpp::Notification notification = {method_name, params};
    endpoint.send_notification(&notification);
}

jsonrpcpp::Response Communicator::call_method(std::string method_name, nlohmann::json params) {
    jsonrpcpp::Response response;
    if (!use_avalon || !connected)
    {
        return response;
    }
    int request_id = client_request_id++;
    jsonrpcpp::Request request = {request_id, method_name, params};
    endpoint.send_request(&request);

    bool found = false;
    while (!found) {
        std::map<int, jsonrpcpp::Response>::iterator iter = responses.find(request_id);
        if (iter != responses.end()) {
            //element found == was found response
            response = iter->second;
            found = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return response;
}

void Communicator::process_requests() {
    if (!use_avalon || !connected || messages.empty()) {return;}

    while (!messages.empty()) {
        std::string msg = messages.front();
        std::cout << "Parsing: " << msg << std::endl;
        auto response = parser.parse(msg);
        if (response->is_response()) {
            endpoint.send_response(response);
        } else {
            jsonrpcpp::request_ptr request = std::dynamic_pointer_cast<jsonrpcpp::Request>(response);
            jsonrpcpp::Error error("Method \"" + request->method() + "\" not found", -32601);
            jsonrpcpp::Response _response(request->id(), error);
            endpoint.send_response(&_response);
        }

        messages.pop();
    }
}

jsonrpcpp::response_ptr execute_george(const jsonrpcpp::Id &id, const jsonrpcpp::Parameter &params) {
    nlohmann::json output;
    try {
        try {
            std::string _george_script;
            const char *george_script;
            char *_output = "";
            int result;

            nlohmann::json json_params = params.to_json();
            _george_script = json_params[0];
            george_script = _george_script.c_str();

            result = TVSendCmd(current_filter, george_script, _output);
            if (_output == "") {
                output = bool(result);
                return std::make_shared<jsonrpcpp::Response>(id, output);
            }
            output = _output;
            return std::make_shared<jsonrpcpp::Response>(id, output);
        } catch (const std::exception &e) {
            output = e.what();
            return std::make_shared<jsonrpcpp::Response>(id, output);
        }
    } catch (const std::exception& e) {
        output = "Crashed";
        return std::make_shared<jsonrpcpp::Response>(id, output);
    }
}

void register_callbacks(){
    parser.register_request_callback("execute_george", execute_george);
}

Communicator communication;

////////////////////////////////////////////////////////////////////////////////////////

// sizes of some GUI components

// 185 is the standard width of most requesters in Aura.
// you should try to respect it, as this makes life easier for the end user
// (for stacking several requesters, and so on...).
#define REQUESTER_W  185
#define REQUESTER_H  130


// ID's of GUI components
#define ID_WORKFILES                10
#define ID_LOADER                   11
#define ID_CREATOR                  12
#define ID_SCENE_INVENTORY          13
#define ID_PUBLISH                  14
#define ID_LIBRARY_LOADER           15

#define TXT_WORKFILES               "Workfiles"
#define TXT_WORKFILES_HELP          "Open workfiles tool"
#define TXT_LOADER                  "Load"
#define TXT_LOADER_HELP             "Open loader tool"
#define TXT_CREATOR                 "Create"
#define TXT_CREATOR_HELP            "Open creator tool"
#define TXT_SCENE_INVENTORY         "Scene inventory"
#define TXT_SCENE_INVENTORY_HELP    "Open scene inventory tool"
#define TXT_PUBLISH                 "Publish"
#define TXT_PUBLISH_HELP            "Open publisher"
#define TXT_LIBRARY_LOADER          "Library"
#define TXT_LIBRARY_LOADER_HELP     "Open library loader tool"


/**************************************************************************************/
//  Localisation

// numbers (like 10011) are IDs in the localized file.
// strings are the default values to use when the ID is not found
// in the localized file (or the localized file doesn't exist).
#define TXT_NAME        "Avalon"

#define TXT_REQUESTER   "Avalon tools"


#define TXT_ERROR01     "Can't Open Requester !"



////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////

// The functions directly called by Aura through the plugin interface



/**************************************************************************************/
// "About" function.


void FAR PASCAL PI_About( PIFilter* iFilter )
{
    char  text[256];

    sprintf( text, "%s %d,%d", iFilter->PIName, iFilter->PIVersion, iFilter->PIRevision );

    // Just open a warning popup with the filter name and version.
    // You can open a much nicer requester if you want.
    TVWarning( iFilter, text );
}


/**************************************************************************************/
// Function called at Aura startup, when the filter is loaded.
// Should do as little as possible to keep Aura's startup time small.

int FAR PASCAL PI_Open( PIFilter* iFilter )
{
    current_filter = iFilter;
    char  tmp[256];

    // Load the .loc file.
    // We don't really cares if it fails here, since we do care in GetLocalString()

    strcpy( iFilter->PIName, TXT_NAME );
    iFilter->PIVersion = 1;
    iFilter->PIRevision = 1;

    // If this plugin was the one open at Aura shutdown, re-open it
    TVReadUserString( iFilter, iFilter->PIName, "Open", tmp, "0", 255 );
    if( atoi( tmp ) )
    {
        PI_Parameters( iFilter, NULL ); // NULL as iArg means "open the requester"
    }

    communication.connect();
    register_callbacks();
    return  1; // OK
}


/**************************************************************************************/
// Aura shutdown: we make all the necessary cleanup

void FAR PASCAL PI_Close( PIFilter* iFilter )
{
    communication.endpoint.close_connection();
}


/**************************************************************************************/
// we have something to do !

int FAR PASCAL PI_Parameters( PIFilter* iFilter, char* iArg )
{
    if( !iArg )
    {
        // If the requester is not open, we open it.
        if( Data.mReq == 0 )
        {
            // We use a variable to contains the vertical position of the buttons.
            // Each time we create a button, we add its size to this variable.
            // This makes it very easy to add/remove/displace buttons in a requester.
            int y_pos = 5;
            int y_offset = 20;

            // Create the requester, without a "menu bar" 'coz we don't need it
            // in this simple example.
            // Also we give 'NULL' as the 'Message Function' for this requester,
            // so all his messages will be sent to PI_Msg.
            // This is an acceptable practice when there are just a few buttons.
            DWORD  req = TVOpenFilterReqEx( iFilter, REQUESTER_W, REQUESTER_H, NULL, NULL, PIRF_STANDARD_REQ, FILTERREQ_NO_TBAR );
            if( req == 0 )
            {
                TVWarning( iFilter, TXT_ERROR01 );
                return  0;
            }
            Data.mReq = req;

            // This is a very simple requester, so we create it's content right here instead
            // of waiting for the PICBREQ_OPEN message...
            // Not recommended for more complex requesters. (see the other examples)

            // Sets the title of the requester.
            TVSetReqTitle( iFilter, Data.mReq, TXT_REQUESTER );

            // Creates a button in the requester (0 as height means use standard value).
            // The ID of the button is ID_FLIPX.
            // The type of the button is PIRBF_BUTTON_NORMAL.
            // The string "Flip X" is written in the middle of the button.
            TVAddButtonReq( iFilter, Data.mReq, 9, y_pos, REQUESTER_W-19, 0, ID_WORKFILES, PIRBF_BUTTON_NORMAL|PIRBF_BUTTON_ACTION, TXT_WORKFILES );
            y_pos += y_offset;
            TVAddButtonReq( iFilter, Data.mReq, 9, y_pos, REQUESTER_W-19, 0, ID_LOADER, PIRBF_BUTTON_NORMAL|PIRBF_BUTTON_ACTION, TXT_LOADER );
            y_pos += y_offset;
            TVAddButtonReq( iFilter, Data.mReq, 9, y_pos, REQUESTER_W-19, 0, ID_CREATOR, PIRBF_BUTTON_NORMAL|PIRBF_BUTTON_ACTION, TXT_CREATOR );
            y_pos += y_offset;
            TVAddButtonReq( iFilter, Data.mReq, 9, y_pos, REQUESTER_W-19, 0, ID_SCENE_INVENTORY, PIRBF_BUTTON_NORMAL|PIRBF_BUTTON_ACTION, TXT_SCENE_INVENTORY );
            y_pos += y_offset;
            TVAddButtonReq( iFilter, Data.mReq, 9, y_pos, REQUESTER_W-19, 0, ID_PUBLISH, PIRBF_BUTTON_NORMAL|PIRBF_BUTTON_ACTION, TXT_PUBLISH );
            y_pos += y_offset;
            TVAddButtonReq( iFilter, Data.mReq, 9, y_pos, REQUESTER_W-19, 0, ID_LIBRARY_LOADER, PIRBF_BUTTON_NORMAL|PIRBF_BUTTON_ACTION, TXT_LIBRARY_LOADER );

            // Put a help messages on it.
            // Help Popup
            TVSetButtonInfoText( iFilter, Data.mReq, ID_WORKFILES, TXT_WORKFILES_HELP );
            TVSetButtonInfoText( iFilter, Data.mReq, ID_LOADER, TXT_LOADER_HELP );
            TVSetButtonInfoText( iFilter, Data.mReq, ID_PUBLISH, TXT_PUBLISH_HELP );
            TVSetButtonInfoText( iFilter, Data.mReq, ID_CREATOR, TXT_CREATOR_HELP );
            TVSetButtonInfoText( iFilter, Data.mReq, ID_SCENE_INVENTORY, TXT_SCENE_INVENTORY_HELP );
            TVSetButtonInfoText( iFilter, Data.mReq, ID_LIBRARY_LOADER, TXT_LIBRARY_LOADER_HELP );
        }
        else
        {
            // If it is already open, we just put it on front of all other requesters.
            TVReqToFront( iFilter, Data.mReq );
        }
    }

    return  1;
}


/**************************************************************************************/
// something happenned that needs our attention.

int FAR PASCAL PI_Msg( PIFilter* iFilter, INTPTR iEvent, INTPTR iReq, INTPTR* iArgs )
{
    // TODO should be filtered to not execute that all the time
    current_filter = iFilter;
    communication.process_requests();

    // what did happen ?
    switch( iEvent )
    {
        // The user just 'clicked' on a normal button
        case PICBREQ_BUTTON_UP:
            switch( iArgs[0] )   // iArgs[0] is the ID of the selected button
            {
                // This call tells Aura to call the following functions in our plugin :
                // PI_SequenceStart, PI_Start, PI_Work, PI_Finish and PI_SequenceFinish
                // in the right order.
                case ID_WORKFILES:
                    communication.call_method("workfiles_tool", nlohmann::json::array());
                    TVExecute( iFilter );
                    break;

                case ID_LOADER:
                    communication.call_method("loader_tool", nlohmann::json::array());
                    TVExecute( iFilter );
                    break;

                case ID_CREATOR:
                    communication.call_method("creator_tool", nlohmann::json::array());
                    TVExecute( iFilter );
                    break;

                case ID_SCENE_INVENTORY:
                    communication.call_method("scene_inventory_tool", nlohmann::json::array());
                    TVExecute( iFilter );
                    break;

                case ID_PUBLISH:
                    communication.call_method("publish_tool", nlohmann::json::array());
                    TVExecute( iFilter );
                    break;

                case ID_LIBRARY_LOADER:
                    communication.call_method("library_loader_tool", nlohmann::json::array());
                    TVExecute( iFilter );
                    break;

                default:
                    break;
            }
            break;

        // The requester was just closed.
        case PICBREQ_CLOSE:
            char  tmp[256];

            // Save the requester state (opened or closed)
            // iArgs[4] contains a flag which tells us if the requester
            // has been closed by the user (flag=0) or by Aura's shutdown (flag=1).
            // If it was by Aura's shutdown, that means this requester was the
            // last one open, so we should reopen this one the next time Aura
            // is started.  Else we won't open it next time.
            sprintf( tmp, "%d", (int)(iArgs[4]) );

            // Save it in Aura's init file.
            TVWriteUserString( iFilter, iFilter->PIName, "Open", tmp );
            break;
    }

    return  1;
}


/**************************************************************************************/
// Start of the 'execution' of the filter for a new sequence.
// - iNumImages contains the total number of frames to be processed.
// Here you should allocate memory that is used for all frames,
// and precompute all the stuff that doesn't change from frame to frame.


int FAR PASCAL PI_SequenceStart( PIFilter* iFilter, int iNumImages )
{
    // In this simple example we don't have anything to allocate/precompute.

    // 1 means 'continue', 0 means 'error, abort' (like 'not enough memory')
    return  1;
}


// Here you should cleanup what you've done in PI_SequenceStart

void FAR PASCAL PI_SequenceFinish( PIFilter* iFilter )
{}


/**************************************************************************************/
// This is called before each frame.
// Here you should allocate memory and precompute all the stuff you can.

int FAR PASCAL PI_Start( PIFilter* iFilter, double iPos, double iSize )
{
    return  1;
}


void FAR PASCAL PI_Finish( PIFilter* iFilter )
{
    // nothing special to cleanup
}


/**************************************************************************************/
// 'Execution' of the filter.
int FAR PASCAL PI_Work( PIFilter* iFilter )
{
    return  1;
}
