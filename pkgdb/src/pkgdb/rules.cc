/* ========================================================================== *
 *
 * @file pkgdb/rules.cc
 *
 * @brief Defines `RulesTreeNode' class, `ScrapeRules` helpers, and interfaces
 *        related related to rules processing
 *        for @a flox::pkgdb::PkgDb::scrape().
 *
 *
 * -------------------------------------------------------------------------- */

#include <optional>
#include <string>

#include <nix/hash.hh>
#include <nlohmann/json.hpp>

#include "flox/core/util.hh"
#include "flox/flake-package.hh"
#include "flox/pkgdb/write.hh"

/* -------------------------------------------------------------------------- */

namespace flox::pkgdb {

/* -------------------------------------------------------------------------- */

extern std::optional<std::string> rulesPath;

/* -------------------------------------------------------------------------- */

std::string
scrapeRuleToString( ScrapeRule rule )
{
  switch ( rule )
    {
      case SR_NONE: return "UNSET";
      case SR_DEFAULT: return "default";
      case SR_ALLOW_PACKAGE: return "allowPackage";
      case SR_DISALLOW_PACKAGE: return "disallowPackage";
      case SR_ALLOW_RECURSIVE: return "allowRecursive";
      case SR_DISALLOW_RECURSIVE: return "disallowRecursive";
      default: return "UNKNOWN";
    }
}


/* -------------------------------------------------------------------------- */

void
RulesTreeNode::addRule( AttrPathGlob & relPath, ScrapeRule rule )
{
  /* Modify our rule. */
  if ( relPath.empty() )
    {
      if ( this->rule != SR_DEFAULT )
        {
          // TODO: Pass abs-path
          throw FloxException(
            "attempted to overwrite existing rule for `" + this->attrName
            + "' with rule `" + scrapeRuleToString( this->rule )
            + "' with new rule `" + scrapeRuleToString( rule ) + "'" );
        }
      traceLog( "assigning rule to `" + scrapeRuleToString( rule ) + "' to `"
                + this->attrName + '\'' );
      this->rule = rule;
      return;
    }
  traceLog( "adding rule to `" + this->attrName + "': `"
            + displayableGlobbedPath( relPath ) + " = "
            + scrapeRuleToString( rule ) + '\'' );

  /* Handle system glob by splitting into 4 recursive calls. */
  if ( ! relPath.front().has_value() )
    {
      traceLog( "splitting system glob into real systems" );
      for ( const auto & system : getDefaultSystems() )
        {
          AttrPathGlob relPathCopy = relPath;
          relPathCopy.front()      = system;
          this->addRule( relPathCopy, rule );
        }
      return;
    }

  std::string attrName = std::move( *relPath.front() );
  // TODO: Use a `std::deque' instead of `std::vector' for efficiency.
  //       This container is only used for `push_back' and `pop_front'.
  //       Removing from the front is inefficient for `std::vector'.
  relPath.erase( relPath.begin() );

  if ( auto it = this->children.find( attrName ); it != this->children.end() )
    {
      traceLog( "found existing child `" + attrName + '\'' );
      /* Add to existing child node. */
      it->second.addRule( relPath, rule );
    }
  else if ( relPath.empty() )
    {
      /* Add leaf node. */
      traceLog( "creating leaf `" + attrName + " = "
                + scrapeRuleToString( rule ) + '\'' );
      this->children.emplace( attrName, RulesTreeNode( attrName, rule ) );
    }
  else
    {
      traceLog( "creating child `" + attrName + '\'' );
      /* Create a new child node. */
      this->children.emplace( attrName, RulesTreeNode( attrName ) );
      this->children.at( attrName ).addRule( relPath, rule );
    }
}


/* -------------------------------------------------------------------------- */

ScrapeRule
RulesTreeNode::getRule( const AttrPath & path ) const
{
  const RulesTreeNode * node = this;
  for ( const auto & attrName : path )
    {
      try
        {
          node = &node->children.at( attrName );
        }
      catch ( const std::out_of_range & err )
        {
          return SR_DEFAULT;
        }
    }
  return node->rule;
}


/* -------------------------------------------------------------------------- */

std::optional<bool>
RulesTreeNode::applyRules( const AttrPath & path ) const
{
  auto rule = this->getRule( path );
  /* Perform lookup in parents if necessary. */
  if ( rule == SR_DEFAULT )
    {
      AttrPath pathCopy = path;
      do {
          pathCopy.pop_back();
          rule = this->getRule( pathCopy );
        }
      while ( ( rule == SR_DEFAULT ) && ( ! pathCopy.empty() ) );
    }

  switch ( rule )
    {
      case SR_ALLOW_PACKAGE: return true;
      case SR_DISALLOW_PACKAGE: return false;
      case SR_ALLOW_RECURSIVE: return true;
      case SR_DISALLOW_RECURSIVE: return false;
      case SR_DEFAULT: return std::nullopt;
      default:
        throw PkgDbException( "encountered unexpected rule `"
                              + scrapeRuleToString( rule ) + '\'' );
    }
}


/* -------------------------------------------------------------------------- */

void
from_json( const nlohmann::json & jfrom, RulesTreeNode & rules )
{
  ScrapeRulesRaw raw = jfrom;
  rules              = static_cast<RulesTreeNode>( raw );
}


/* -------------------------------------------------------------------------- */

void
to_json( nlohmann::json & jto, const RulesTreeNode & rules )
{
  jto           = nlohmann::json::object();
  jto["__rule"] = scrapeRuleToString( rules.rule );
  for ( const auto & [name, child] : rules.children )
    {
      nlohmann::json jchild;
      to_json( jchild, child );
      jto[name] = jchild;
    }
}


/* -------------------------------------------------------------------------- */

std::string
RulesTreeNode::getHash() const
{
  std::string raw  = nlohmann::json( *this ).dump();
  nix::Hash   hash = nix::hashString( nix::HashType::htSHA256, raw );
  return hash.to_string( nix::Base16, false );
}


/* -------------------------------------------------------------------------- */

RulesTreeNode::RulesTreeNode( ScrapeRulesRaw raw )
{
  for ( const auto & path : raw.allowPackage )
    {
      AttrPathGlob pathCopy( std::move( path ) );
      this->addRule( pathCopy, SR_ALLOW_PACKAGE );
    }
  for ( const auto & path : raw.disallowPackage )
    {
      AttrPathGlob pathCopy( std::move( path ) );
      this->addRule( pathCopy, SR_DISALLOW_PACKAGE );
    }
  for ( const auto & path : raw.allowRecursive )
    {
      AttrPathGlob pathCopy( std::move( path ) );
      this->addRule( pathCopy, SR_ALLOW_RECURSIVE );
    }
  for ( const auto & path : raw.disallowRecursive )
    {
      AttrPathGlob pathCopy( std::move( path ) );
      this->addRule( pathCopy, SR_DISALLOW_RECURSIVE );
    }
}


/* -------------------------------------------------------------------------- */

void
from_json( const nlohmann::json & jfrom, ScrapeRulesRaw & rules )
{
  for ( const auto & [key, value] : jfrom.items() )
    {
      if ( key == "allowPackage" )
        {
          for ( const auto & path : value )
            {
              try
                {
                  rules.allowPackage.emplace_back( path );
                }
              catch ( nlohmann::json::exception & err )
                {
                  throw PkgDbException(
                    "couldn't interpret field `allowPackage." + key + "': ",
                    flox::extract_json_errmsg( err ) );
                }
            }
        }
      else if ( key == "disallowPackage" )
        {
          for ( const auto & path : value )
            {
              try
                {
                  rules.disallowPackage.emplace_back( path );
                }
              catch ( nlohmann::json::exception & err )
                {
                  throw PkgDbException(
                    "couldn't interpret field `disallowPackage." + key + "': ",
                    flox::extract_json_errmsg( err ) );
                }
            }
        }
      else if ( key == "allowRecursive" )
        {
          for ( const auto & path : value )
            {
              try
                {
                  rules.allowRecursive.emplace_back( path );
                }
              catch ( nlohmann::json::exception & err )
                {
                  throw PkgDbException(
                    "couldn't interpret field `allowRecursive." + key + "': ",
                    flox::extract_json_errmsg( err ) );
                }
            }
        }
      else if ( key == "disallowRecursive" )
        {
          for ( const auto & path : value )
            {
              try
                {
                  rules.disallowRecursive.emplace_back( path );
                }
              catch ( nlohmann::json::exception & err )
                {
                  throw PkgDbException(
                    "couldn't interpret field `disallowRecursive." + key
                      + "': ",
                    flox::extract_json_errmsg( err ) );
                }
            }
        }
      else { throw FloxException( "unknown scrape rule: `" + key + "'" ); }
    }
}


/* -------------------------------------------------------------------------- */

const RulesTreeNode &
getDefaultRules()
{
  static std::optional<RulesTreeNode> rules;
  if ( ! rules.has_value() )
    {
      ScrapeRulesRaw raw = nlohmann::json::parse(
#include "./rules.json.hh"
      );
      rules = RulesTreeNode( std::move( raw ) );
    }
  return *rules;
}


/* -------------------------------------------------------------------------- */

}  // namespace flox::pkgdb


/* -------------------------------------------------------------------------- *
 *
 *
 *
 * ========================================================================== */