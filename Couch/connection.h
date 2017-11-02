#ifndef CPPCOUCH_CONNECTION_H
#define CPPCOUCH_CONNECTION_H

#include "database.h"
#include "communication.h"
#include "user.h"

#include "shared.h"

namespace couchdb
{
    /* Connection class - The parent object for communicating with CouchDB. Any program
     * communicating with CouchDB must have a Connection object to handle communications
     * to and from the specialized document/database/view/etc. classes.
     *
     * IMPORTANT NOTE: Any document/database/view/etc. classes require their parent Connection
     * object to be valid at all times. This is because of internal referencing to the Communication
     * object.
     */

    template<typename http_client> class locator;
    template<typename http_client> class database;
    template<typename http_client> class document;
    template<typename http_client> class cluster_connection;
    template<typename http_client> class node_connection;

    template<typename http_client>
    class connection
    {
        friend class locator<http_client>;
        friend class database<http_client>;
        friend class document<http_client>;

        typedef communication<http_client> base;
        typedef database<http_client> database_type;

    public:
        connection(std::shared_ptr<base> _comm = std::shared_ptr<base>())
            : comm(_comm)
        {}
        connection(std::shared_ptr<base> _comm,
                   const std::string &url,
                   const user &_user = user(),
                   auth_type authType = auth_none)
            : comm(_comm? _comm: std::make_shared<base>(http_client(), url, _user, authType))
        {
            comm->set_server_url(url);
        }
        connection(http_client _comm,
                   const std::string &url,
                   const user &_user = user(),
                   auth_type authType = auth_none)
            : comm(std::make_shared<base>(_comm, url, _user, authType))
        {}
        connection(const std::string &url,
                   const user &_user = user(),
                   auth_type authType = auth_none)
            : comm(std::make_shared<base>(http_client(), url, _user, authType))
        {}
        virtual ~connection() {}

        base &lowest_level() {return comm;}

        // Get and set communication timeouts, in milliseconds
        virtual typename base::http_client_timeout_duration_t get_timeout() const {return comm->get_timeout();}
        virtual void set_timeout(typename base::http_client_timeout_duration_t timeout) {comm->set_timeout(timeout);}

        // Returns the version of CouchDB
        virtual std::string get_couchdb_version()
        {
            get_couchdb_info();
            return couchDBVersion;
        }

        // Returns 'count' UUIDs generated by CouchDB if possible
        virtual std::vector<std::string> get_uuids(size_t count = 10)
        {
            json::value response = comm->get_data("/_uuids?count=" + std::to_string(count));
            if (!response.is_object())
                throw error(error::bad_response);

            response = response["uuids"];
            if (!response.is_array())
                throw error(error::bad_response);

            std::vector<std::string> uuids;
            for (auto val: response.get_array())
                uuids.push_back(val.get_string());

            return uuids;
        }

        // Lists the names of all databases, except those beginning with '_' and shards
        // (which are assumed to be reserved)
        virtual std::vector<std::string> list_db_names()
        {
            json::value response = comm->get_data("/_all_dbs");
            if (!response.is_array())
                throw error(error::database_unavailable);

            std::vector<std::string> dbs;
            for (auto item: response.get_array())
            {
                if (item.get_string().find('_') != 0 && item.get_string().find("shards/") != 0)
                    dbs.push_back(item.get_string());
            }

            return dbs;
        }

        // Lists the names of all databases, including those beginning with '_' and shards
        virtual std::vector<std::string> list_all_db_names()
        {
            json::value response = comm->get_data("/_all_dbs");
            if (!response.is_array())
                throw error(error::database_unavailable);

            std::vector<std::string> dbs;
            for (auto item: response.get_array())
                dbs.push_back(item.get_string());

            return dbs;
        }

        // Lists all database objects, except those beginning with '_' and shards
        // (which are assumed to be reserved)
        virtual std::vector<database_type> list_dbs()
        {
            json::value response = comm->get_data("/_all_dbs");
            if (!response.is_array())
                throw error(error::database_unavailable);

            std::vector<database_type> dbs;
            for (auto item: response.get_array())
            {
                if (item.get_string().find('_') != 0 && item.get_string().find("shards/") != 0)
                    dbs.push_back(database_type(comm, item.get_string()));
            }

            return dbs;
        }

        // Lists all database objects, including those beginning with '_'
        virtual std::vector<database_type> list_all_dbs()
        {
            json::value response = comm->get_data("/_all_dbs");
            if (!response.is_array())
                throw error(error::database_unavailable);

            std::vector<database_type> dbs;
            for (auto item: response.get_array())
                dbs.push_back(database_type(comm, item.get_string()));

            return dbs;
        }

        // Returns the database with the given name.
        virtual database_type get_db(const std::string &db)
        {
            comm->get_raw_data("/" + url_encode(db), "HEAD");
            return database_type(comm, db);
        }

        // Returns true if the database exists
        virtual bool db_exists(const std::string &db)
        {
            return database_type(comm, db).exists();
        }

        // Creates a database with given name if possible, and returns it
        virtual database_type create_db(const std::string &db)
        {
            json::value response = comm->get_data("/" + url_encode(db), "PUT");
            if (!response.is_object())
                throw error(error::database_unavailable);

            if (response.is_member("error"))
            {
#ifdef CPPCOUCH_DEBUG
                std::cout << "Unable to create database \"" + db + "\": " + response["reason"].get_string();
#endif
                throw error(error::database_not_creatable, response["reason"].get_string());
            }

            if (!response["ok"].get_bool())
                throw error(error::database_not_creatable);

            return database_type(comm, db);
        }

        // Ensures the database with given name exists, and returns it
        virtual database_type ensure_db_exists(const std::string &db)
        {
            try {return get_db(db);}
            catch (error &e) {if (e.type() != error::content_not_found) throw;}

            return create_db(db);
        }

        // Ensures the database with given name is deleted
        virtual connection &ensure_db_is_deleted(const std::string &db)
        {
            try {return remove_db(db);}
            catch (error &e) {if (e.type() != error::content_not_found && e.type() != error::database_not_deletable) throw;}

            return *this;
        }

        // Deletes a database with given name if possible
        // NOTE: this is an IRREVERSIBLE operation!
        virtual connection &remove_db(const std::string &db)
        {
            json::value response = comm->get_data("/" + url_encode(db), "DELETE");
            if (!response.is_object())
                throw error(error::database_not_deletable);

            if (response.is_member("error"))
            {
#ifdef CPPCOUCH_DEBUG
                std::cout << "Unable to delete database \"" + db + "\": " + response["reason"].get_string();
#endif
                throw error(error::database_not_deletable, response["reason"].get_string());
            }

            if (!response["ok"].get_bool())
                throw error(error::database_not_deletable);

            return *this;
        }

        // Returns the URL of the CouchDB server for this connection
        virtual std::string get_server_url() const {return comm->get_server_url();}
        // Sets the URL of the CouchDB server for this connection
        virtual connection &set_server_url(const std::string &url) {comm->set_server_url(url); return *this;}

        // Returns the user authentication object for this connection
        virtual user get_user() const {return comm->get_user();}
        // Sets the user authentication object for this connection
        virtual connection &set_user(const user &_user) {comm->set_user(_user); return *this;}

        // Gets the authentication type for this connection
        virtual auth_type get_auth_type() const {return comm->get_auth_type();}
        // Gets the readable authentication type for this connection
        virtual std::string get_auth_type_readable() const {return comm->get_auth_type_readable();}
        // Sets the authentication type for this connection
        virtual connection &set_auth_type(auth_type type) {comm->set_auth_type(type); return *this;}
        // Sets the readable authentication type for this connection ("none", "basic", or "auth", case insensitive)
        virtual connection &set_auth_type(const std::string &type) {comm->set_auth_type(type); return *this;}

        // Tries to log in to the CouchDB server
        virtual connection &login()
        {
            auth_type t = comm->get_auth_type();
            switch (t)
            {
                case auth_basic:
                case auth_cookie:
                {
                    json::value response, obj;

                    obj["name"] = comm->get_user().username();
                    obj["password"] = comm->get_user().password();

                    comm->set_auth_type(auth_basic); // Clear so communicator won't try to use the cookie to log in right away
                    response = comm->get_data("/_session", "POST", json_to_string(obj));

                    // We actually performed only one type of login, and got a cookie from the server that is now embedded in the communicator.
                    // If we use basic auth, we want to get rid of this cookie and log out immediately. Otherwise, that is our cookie for the session.
                    if (t == auth_basic)
                    {
                        comm->set_auth_type(auth_cookie); // If basic auth, we now log out to clear the cookie session
                        // If logout() fails, we want this class to be in a well-defined state.
                        try {this->logout();}
                        catch (error) {comm->set_auth_type(t); throw;}
                    }

                    comm->set_auth_type(t); // Reset to original value
                }
                case auth_none:
                default:
                    break;
            }

            return *this;
        }

        // Tries to return the login information for the current session
        virtual json::value get_login_info()
        {
            switch (comm->get_auth_type())
            {
                case auth_cookie:
                    return comm->get_data("/_session");
                default:
                    return json::value();
            }
        }

        // Logs out from the CouchDB server
        virtual connection &logout()
        {
            switch (comm->get_auth_type())
            {
                case auth_cookie:
                    comm->get_data("/_session", "DELETE");
                default:
                    break;
            }

            return *this;
        }

        // Tries to create a new CouchDB user
        virtual json::value create_user(const std::string &name, const std::string &pass, const json::value &roles = json::array_t() /* Array */)
        {
            json::value obj;

            obj["name"] = name;
            if (pass.empty())
                obj["password"] = json::value();
            else
                obj["password"] = pass;
            if (roles.is_array())
                obj["roles"] = roles;
            obj["type"] = "user";

            return comm->get_data("/_users/org.couchdb.user:" + url_encode(name), "PUT", json_to_string(obj));
        }

        virtual std::vector<std::string> list_user_names()
        {
            std::vector<std::string> list;
            json::value response = comm->get_data("/_users/_all_docs", "GET");
            if (!response.is_object() || !response["rows"].is_array())
                throw error(error::bad_response);

            response = response["rows"];
            for (auto row: response.get_array())
            {
                std::string prefix = "org.couchdb.user:";
                std::string name = row["id"].get_string();

                if (name.find('_') == 0)
                    continue;

                if (name.find(prefix) == 0)
                    name.erase(0, prefix.size());

                if (!name.empty())
                    list.push_back(name);
            }

            return list;
        }

        // Tries to create a new CouchDB user
        virtual json::value create_user(const user &_user, const json::value &roles = json::array_t())
        {
            return create_user(_user.username(), _user.password(), roles);
        }

        // Tries to get information about a CouchDB user
        virtual json::value get_user_info(const std::string &name)
        {
            return comm->get_data("/_users/org.couchdb.user:" + url_encode(name), "GET");
        }

        // Tries to delete an existing CouchDB user
        virtual json::value delete_user(const std::string &name)
        {
            return comm->get_data("/_users/org.couchdb.user:" + url_encode(name), "DELETE");
        }

        // Returns response from /_active_tasks endpoint
        json::value get_active_tasks()
        {
            json::value response = this->comm->get_data("/_active_tasks", "GET");
            if (!response.is_array())
                throw error(error::bad_response);
            return response;
        }

        // Returns true if this connection supports clustering
        virtual bool get_supports_clusters() {return get_major_version() >= 2;}

        // Returns a cluster connection if this connection supports clustering, otherwise returns a NULL pointer
        // If running on a CouchDB version older than 2.0.0, this will return a NULL pointer.
        // If running on CouchDB >= 2.0.0, this will return a cluster connection that can be used to obtain node information
        // node_local_port is the port, accessible from the localhost, that provides access to the internal node configuration
        virtual std::shared_ptr<cluster_connection<http_client>> upgrade_to_cluster_connection(unsigned short node_local_port = couchdb::local_cluster_node_port())
        {
            if (get_supports_clusters())
                return std::make_shared<cluster_connection<http_client>>(cluster_connection<http_client>(node_local_port, comm));
            return {};
        }

        // Returns a node connection if this connection does not support clustering, otherwise returns a NULL pointer
        // If running on a CouchDB version older than 2.0.0, this will return a node connection for the server itself, without a node name.
        // If running on CouchDB >= 2.0.0, this will return a node connection for the first node in the cluster, equal to `*upgrade_to_cluster_connection()->begin()`
        // node_local_port is the port, accessible from the localhost, that provides access to the internal node configuration
        virtual std::shared_ptr<node_connection<http_client>> upgrade_to_node_connection(unsigned short node_local_port = couchdb::local_cluster_node_port())
        {
            if (!get_supports_clusters())
                return std::make_shared<node_connection<http_client>>(node_connection<http_client>(node_local_port, std::string(), comm));
            else
                return *cluster_connection<http_client>(node_local_port, comm).begin();
        }

        // Returns the version of the current CouchDB instance, or -1 if unknown
        virtual int get_major_version()
        {
            get_couchdb_info();
            return couchdb_major;
        }

    protected:
        virtual void get_couchdb_info()
        {
            json::value response = comm->get_data("", "GET", "", true);
            if (!response.is_object())
                throw error(error::bad_response);

            couchDBVersion = response["version"].get_string();
            std::string copy(couchDBVersion);
            if (copy.find('.') != std::string::npos)
                copy.erase(copy.find('.'));

            std::istringstream stream(copy);
            stream >> couchdb_major;
            if (!stream)
                couchdb_major = -1;
        }

        std::shared_ptr<base> comm;
        std::string couchDBVersion;
        int couchdb_major;
    };

    template<typename http_client, typename... args>
    std::shared_ptr<connection<typename http_client::type>> make_connection(args... arguments)
    {
        return std::make_shared<connection<http_client>>(arguments...);
    }

    template<typename http_client, typename... args>
    std::shared_ptr<connection<typename http_client::type>> make_connection(const http_client &client, args... arguments)
    {
        return std::make_shared<connection<http_client>>(client, arguments...);
    }
}

#endif // CPPCOUCH_CONNECTION_H
